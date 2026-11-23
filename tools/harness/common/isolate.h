#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

// Frame times on an SMT processor depend on what runs on the other logical
// processor of the frame thread's core. The driver's and the DLL's threads
// land there at random, which changed GIMI frame times by up to 18% between
// runs of the same DLL. isolate_frame_thread() puts the calling thread on
// logical processor XH_CORE (default 4) and keeps the other threads of the
// process off the logical processors of that core: the threads that exist
// get an affinity mask, and when Windows reports CPU sets, threads made later
// get the process's default CPU sets. Without CPU sets, processors 2k and
// 2k+1 are taken as one core and threads made later are not restricted.
// XH_CORE=-1 turns it off.
static void print_once(bool &printed, const char *format, const char *text)
{
	if (!printed)
		printf(format, text);
	printed = true;
}

static int isolated_core()
{
	char text[16] = {};
	if (!GetEnvironmentVariableA("XH_CORE", text, sizeof(text)) || !text[0])
		return 4;
	char *end = NULL;
	long core = strtol(text, &end, 10);
	if (*end) {
		static bool printed;
		print_once(printed, "XH_CORE=%s is not a number, the frame thread is not isolated\n", text);
		return -1;
	}
	return (int)core;
}

static void isolate_frame_thread()
{
	int core = isolated_core();
	DWORD_PTR process_mask = 0, system_mask = 0;
	GROUP_AFFINITY group = {};
	if (core < 0 || core >= (int)(sizeof(DWORD_PTR) * 8) ||
	    !GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) ||
	    !GetThreadGroupAffinity(GetCurrentThread(), &group))
		return;
	DWORD_PTR own = (DWORD_PTR)1 << core, pair = 0;

	std::vector<char> sets;
	ULONG bytes = 0;
	GetSystemCpuSetInformation(NULL, 0, &bytes, GetCurrentProcess(), 0);
	if (bytes) {
		sets.resize(bytes);
		if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)sets.data(), bytes, &bytes, GetCurrentProcess(), 0))
			sets.clear();
	}
	auto each_set = [&](auto &&visit) {
		for (size_t at = 0; at + sizeof(DWORD) * 2 <= sets.size();) {
			auto *info = (PSYSTEM_CPU_SET_INFORMATION)(sets.data() + at);
			if (!info->Size)
				break;
			if (info->Type == CpuSetInformation && info->CpuSet.Group == group.Group &&
			    info->CpuSet.LogicalProcessorIndex < sizeof(DWORD_PTR) * 8)
				visit(info->CpuSet);
			at += info->Size;
		}
	};
	int own_core = -1;
	each_set([&](auto &set) { if (set.LogicalProcessorIndex == core) own_core = set.CoreIndex; });
	each_set([&](auto &set) { if (set.CoreIndex == own_core) pair |= (DWORD_PTR)1 << set.LogicalProcessorIndex; });
	if (!(pair & own))
		pair = (DWORD_PTR)3 << (core & ~1);
	DWORD_PTR others = process_mask & ~pair;
	if (!(process_mask & own) || !others) {
		static bool warned;
		if (!warned)
			printf("XH_CORE=%d is not usable on this processor, the frame thread is not isolated\n", core);
		warned = true;
		return;
	}

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	SetThreadAffinityMask(GetCurrentThread(), own);

	std::vector<ULONG> own_set, other_sets;
	each_set([&](auto &set) {
		DWORD_PTR bit = (DWORD_PTR)1 << set.LogicalProcessorIndex;
		if (bit == own)
			own_set.push_back(set.Id);
		else if (bit & others)
			other_sets.push_back(set.Id);
	});
	bool later_threads_kept_off = own_set.size() == 1 && !other_sets.empty() &&
		SetThreadSelectedCpuSets(GetCurrentThread(), own_set.data(), 1) &&
		SetProcessDefaultCpuSets(GetCurrentProcess(), other_sets.data(), (ULONG)other_sets.size());
	if (!later_threads_kept_off) {
		static bool printed;
		print_once(printed, "%s", "CPU sets unavailable, threads made later are not kept off the frame thread's core\n");
	}

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return;
	THREADENTRY32 entry = { sizeof(entry) };
	for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
			continue;
		HANDLE thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
		if (thread) {
			SetThreadAffinityMask(thread, others);
			CloseHandle(thread);
		}
	}
	CloseHandle(snapshot);
}

// Other processes still slow the frame thread down: in GIMI runs the frame
// time rose with the share of the processors that other processes used.
// print_other_processes_busy() prints that share since the start snapshot,
// so that a time measured while the machine was busy can be recognized.
struct MachineLoad {
	ULONG64 all = 0, idle = 0, own = 0;
};

static ULONG64 filetime_ticks(FILETIME t)
{
	return (ULONG64)t.dwHighDateTime << 32 | t.dwLowDateTime;
}

static MachineLoad machine_load_now()
{
	MachineLoad load;
	FILETIME idle, kernel, user, created, exited;
	if (GetSystemTimes(&idle, &kernel, &user)) {
		load.idle = filetime_ticks(idle);
		load.all = filetime_ticks(kernel) + filetime_ticks(user);
	}
	if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
		load.own = filetime_ticks(kernel) + filetime_ticks(user);
	return load;
}

static void print_other_processes_busy(const MachineLoad &start)
{
	MachineLoad now = machine_load_now();
	double all = (double)(now.all - start.all);
	if (all <= 0)
		return;
	double busy = all - (double)(now.idle - start.idle) - (double)(now.own - start.own);
	printf("other processes: %.0f%% of the logical processors busy during the timed frames\n", busy > 0 ? 100 * busy / all : 0.0);
}
