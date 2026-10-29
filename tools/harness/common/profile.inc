#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
// A sampling profiler for the thread that renders: where its instruction
// pointer is, a few thousand times a second, by function and by line
static HANDLE profiled_thread;
static volatile bool profiling, profile_stop;
static std::vector<DWORD64> samples;
// The return addresses above each sample, ended by a 0
static std::vector<DWORD64> stacks;

// The unwind tables of the modules, looked up without the loader's locks:
// RtlLookupFunctionEntry takes one, and the thread stopped for a sample may
// hold it (loading a DLL), which hung the harness. Modules loaded since the
// last refresh end a stack early and are picked up after the thread runs on.
struct UnwindTable { DWORD64 base, end; const RUNTIME_FUNCTION *functions; DWORD count; };
static std::vector<UnwindTable> unwind_tables;
static bool unwind_tables_stale = true;

static void refresh_unwind_tables()
{
	HMODULE modules[1024];
	DWORD needed = 0;

	unwind_tables.clear();
	if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
		return;
	for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) {
		BYTE *base = (BYTE*)modules[i];
		IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
		IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		if (!dir.Size)
			continue;
		unwind_tables.push_back({ (DWORD64)base, (DWORD64)base + nt->OptionalHeader.SizeOfImage,
			(const RUNTIME_FUNCTION*)(base + dir.VirtualAddress), (DWORD)(dir.Size / sizeof(RUNTIME_FUNCTION)) });
	}
	unwind_tables_stale = false;
}

static const RUNTIME_FUNCTION *find_unwind(DWORD64 address, DWORD64 *image, bool *known)
{
	*known = false;
	for (const UnwindTable &t : unwind_tables) {
		if (address < t.base || address >= t.end)
			continue;
		*known = true;
		*image = t.base;
		DWORD rva = (DWORD)(address - t.base), lo = 0, hi = t.count;
		while (lo < hi) {
			DWORD mid = (lo + hi) / 2;
			if (rva < t.functions[mid].BeginAddress)
				hi = mid;
			else if (rva >= t.functions[mid].EndAddress)
				lo = mid + 1;
			else {
				const RUNTIME_FUNCTION *f = &t.functions[mid];
				if (f->UnwindData & 1)
					f = (const RUNTIME_FUNCTION*)(t.base + (f->UnwindData & ~1u));
				return f;
			}
		}
		return NULL;
	}
	return NULL;
}

static DWORD WINAPI profiler(void *)
{
	CONTEXT context;

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	while (!profile_stop) {
		LARGE_INTEGER freq, start, now;

		// Sleep is too coarse for the rate wanted
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);
		do {
			YieldProcessor();
			QueryPerformanceCounter(&now);
		} while ((now.QuadPart - start.QuadPart) * 8000 < freq.QuadPart);

		if (!profiling)
			continue;
		if (unwind_tables_stale)
			refresh_unwind_tables();
		if (SuspendThread(profiled_thread) == (DWORD)-1)
			continue;
		// Nothing may allocate while the thread is stopped: it may hold the
		// heap lock. What is found goes to the vectors once it runs again.
		DWORD64 found[26];
		int count = 0;

		context.ContextFlags = CONTEXT_FULL;
		if (GetThreadContext(profiled_thread, &context)) {
			int depth;

			found[count++] = context.Rip;
			// The thread is stopped, so its stack can be walked from here
			for (depth = 0; depth < 24; depth++) {
				DWORD64 image = 0, establisher = 0;
				void *handler = NULL;
				bool known;
				RUNTIME_FUNCTION *function = (RUNTIME_FUNCTION*)find_unwind(context.Rip, &image, &known);

				if (!known) {
					unwind_tables_stale = true;
					break;
				}
				if (!function) {
					if (depth || !context.Rsp || IsBadReadPtr((void*)context.Rsp, 8))
						break;
					context.Rip = *(DWORD64*)context.Rsp;
					context.Rsp += 8;
				} else {
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler, &establisher, NULL);
				}
				if (!context.Rip)
					break;
				found[count++] = context.Rip;
			}
		}
		ResumeThread(profiled_thread);
		if (count) {
			samples.push_back(found[0]);
			stacks.insert(stacks.end(), found + 1, found + count);
			stacks.push_back(0);
		}
	}
	return 0;
}

static HANDLE profiler_thread;

// Samples the calling thread while profiling is set
static void start_profile()
{
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &profiled_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
	profiler_thread = CreateThread(NULL, 0, profiler, NULL, 0, NULL);
}

// The profiler may be in the middle of a sample: samples and stacks are only
// read once it has ended
static void stop_profile()
{
	profile_stop = true;
	if (profiler_thread) {
		WaitForSingleObject(profiler_thread, INFINITE);
		CloseHandle(profiler_thread);
		profiler_thread = NULL;
	}
}

// The DLL has the name of the system's: tell the two apart by where they are
static bool system_copy(const IMAGEHLP_MODULE64 &module)
{
	return !_stricmp(module.ModuleName, "d3d11") && (strstr(module.LoadedImageName, "ystem32") || strstr(module.LoadedImageName, "YSTEM32"));
}

static void report_profile()
{
	HANDLE process = GetCurrentProcess();
	std::map<std::string, UINT> functions, lines, modules;
	std::vector<std::pair<UINT, std::string>> sorted;
	char buffer[sizeof(SYMBOL_INFO) + 512];
	SYMBOL_INFO *symbol = (SYMBOL_INFO*)buffer;
	size_t n;

	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(process, NULL, TRUE);

	for (DWORD64 address : samples) {
		IMAGEHLP_MODULE64 module = { sizeof(module) };
		IMAGEHLP_LINE64 line = { sizeof(line) };
		DWORD64 displacement = 0;
		DWORD line_displacement = 0;
		std::string module_name = "?", name;
		char text[700];

		if (SymGetModuleInfo64(process, address, &module))
			module_name = system_copy(module) ? "d3d11sys" : module.ModuleName;
		modules[module_name]++;

		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 500;
		if (SymFromAddr(process, address, &displacement, symbol))
			name = module_name + "!" + symbol->Name;
		else
			name = module_name + "!?";
		functions[name]++;

		if (module_name == "d3d11" && SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
			const char *file = strrchr(line.FileName, 92);

			sprintf_s(text, "%s:%lu (%s)", file ? file + 1 : line.FileName, line.LineNumber, symbol->Name);
			lines[text]++;
		}
	}

	// Inclusive: every function of the DLL on the stack of a sample, once, and who called the
	// function the sample was in
	{
		std::map<std::string, UINT> inclusive;
		std::map<std::string, std::map<std::string, UINT>> callers;
		std::map<DWORD64, std::string> names;
		size_t at = 0;

		auto name_of = [&](DWORD64 address) -> const std::string& {
			auto found = names.find(address);
			if (found != names.end())
				return found->second;
			IMAGEHLP_MODULE64 module = { sizeof(module) };
			DWORD64 displacement = 0;
			std::string module_name = "?";
			if (SymGetModuleInfo64(process, address, &module))
				module_name = system_copy(module) ? "d3d11sys" : module.ModuleName;
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 500;
			return names[address] = module_name + "!" + (SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : "?");
		};

		// PROFILE_FOCUS=name,name: for the functions with one of the names
		// in theirs, who has them on the stack and what they have below
		std::vector<std::string> focus;
		std::map<std::string, std::map<std::string, UINT>> above, below;
		if (const char *wanted = getenv("PROFILE_FOCUS")) {
			std::string all = wanted;
			for (size_t from = 0; from <= all.size();) {
				size_t to = all.find(',', from);
				if (to == std::string::npos)
					to = all.size();
				focus.push_back(all.substr(from, to - from));
				from = to + 1;
			}
		}

		// Time outside the DLL, by the line of the DLL nearest on the stack
		{
			std::map<std::string, UINT> sites;
			size_t frame = 0;
			UINT outside = 0;
			for (DWORD64 address : samples) {
				bool inside = !name_of(address).compare(0, 6, "d3d11!");
				bool found = false;
				for (; frame < stacks.size() && stacks[frame]; frame++) {
					if (inside || found || name_of(stacks[frame]).compare(0, 6, "d3d11!"))
						continue;
					IMAGEHLP_LINE64 line = { sizeof(line) };
					DWORD line_displacement = 0;
					char text[700];
					found = true;
					if (SymGetLineFromAddr64(process, stacks[frame] - 1, &line_displacement, &line)) {
						const char *file = strrchr(line.FileName, 92);
						sprintf_s(text, "%s:%lu (%.80s)", file ? file + 1 : line.FileName, line.LineNumber, name_of(stacks[frame]).c_str() + 6);
					} else {
						sprintf_s(text, "%.100s", name_of(stacks[frame]).c_str());
					}
					sites[text]++;
				}
				frame++;
				if (!inside) {
					outside++;
					if (!found)
						sites["(no line of the DLL on the stack: the game's own calls)"]++;
				}
			}
			sorted.clear();
			for (auto &i : sites)
				sorted.push_back({ i.second, i.first });
			std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
			printf("--- time outside the DLL (%.2f%%), by the line of the DLL that called out\n", outside * 100.0 / samples.size());
			for (n = 0; n < sorted.size() && n < 40; n++)
				printf("%6.2f%%  %.150s\n", sorted[n].first * 100.0 / samples.size(), sorted[n].second.c_str());
		}

		for (DWORD64 address : samples) {
			std::vector<std::string> seen, chain;
			std::string leaf = name_of(address);
			bool first = true;

			seen.push_back(leaf);
			chain.push_back(leaf);
			for (size_t frame = at; !focus.empty() && frame < stacks.size() && stacks[frame]; frame++) {
				const std::string &name = name_of(stacks[frame]);
				if (name != chain.back())
					chain.push_back(name);
			}
			for (const std::string &wanted : focus) {
				for (size_t i = 0; i < chain.size(); i++) {
					if (chain[i].find(wanted) == std::string::npos)
						continue;
					above[wanted][(i + 1 < chain.size() ? chain[i + 1] : "-") + " <- " + (i + 2 < chain.size() ? chain[i + 2] : "-")]++;
					below[wanted][i ? chain[i - 1] : "(itself)"]++;
					break;
				}
			}
			for (; at < stacks.size() && stacks[at]; at++) {
				const std::string &name = name_of(stacks[at]);

				if (first && name != leaf) {
					callers[leaf][name]++;
					first = false;
				}
				if (std::find(seen.begin(), seen.end(), name) == seen.end())
					seen.push_back(name);
			}
			at++;
			for (const std::string &name : seen) {
				if (!name.compare(0, 6, "d3d11!"))
					inclusive[name]++;
			}
		}

		for (const std::string &wanted : focus) {
			for (int side = 0; side < 2; side++) {
				sorted.clear();
				for (auto &i : (side ? below : above)[wanted])
					sorted.push_back({ i.second, i.first });
				std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
				printf("--- %s %s\n", side ? "called by" : "callers of", wanted.c_str());
				for (n = 0; n < sorted.size() && n < 14; n++)
					printf("%6.2f%%  %.150s\n", sorted[n].first * 100.0 / samples.size(), sorted[n].second.c_str());
			}
		}

		sorted.clear();
		for (auto &i : inclusive)
			sorted.push_back({ i.second, i.first });
		std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
		printf("--- inclusive, by function of d3d11.dll\n");
		for (n = 0; n < sorted.size() && n < 110; n++)
			printf("%6.2f%%  %.110s\n", sorted[n].first * 100.0 / samples.size(), sorted[n].second.c_str());

		sorted.clear();
		for (auto &i : functions)
			sorted.push_back({ i.second, i.first });
		std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
		printf("--- callers of the functions sampled most\n");
		for (n = 0; n < sorted.size() && n < 45; n++) {
			std::vector<std::pair<UINT, std::string>> who;

			for (auto &i : callers[sorted[n].second])
				who.push_back({ i.second, i.first });
			std::sort(who.begin(), who.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
			printf("%6.2f%%  %.90s\n", sorted[n].first * 100.0 / samples.size(), sorted[n].second.c_str());
			for (size_t w = 0; w < who.size() && w < 5; w++)
				printf("           %5.2f%%  %.90s\n", who[w].first * 100.0 / samples.size(), who[w].second.c_str());
		}
	}

	printf("%zu samples\n", samples.size());
	for (int pass = 0; pass < 3; pass++) {
		std::map<std::string, UINT> &map = pass == 0 ? modules : pass == 1 ? functions : lines;
		size_t limit = pass == 0 ? 12 : 160;

		sorted.clear();
		for (auto &i : map)
			sorted.push_back({ i.second, i.first });
		std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
		printf("--- by %s\n", pass == 0 ? "module" : pass == 1 ? "function" : "line of d3d11.dll");
		for (n = 0; n < sorted.size() && n < limit; n++)
			printf("%6.2f%%  %s\n", sorted[n].first * 100.0 / samples.size(), sorted[n].second.c_str());
	}
}
