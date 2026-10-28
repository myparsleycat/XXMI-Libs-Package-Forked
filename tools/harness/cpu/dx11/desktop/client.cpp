// A console client of the desktop channel (docs/desktop-channel.md), for
// checking a game by hand before Nahida Desktop speaks the protocol. Connects
// to the pipe of the game, checks that its server is that process, and prints
// every frame it gets as JSON, to the console and to the log file. Reconnects
// when the connection ends, until the game exits.
//
//   client.exe [pid] [--log <file>]
//
// Without a pid it takes the first nahida-migoto-<pid> pipe there is, waiting
// for one. Commands, one a line:
//
//   (empty) or pick     pick under the mouse cursor
//   pick <x> <y>        pick at a normalized position of the window
//   get <name> ...      GetVars, for example get $\Mods\a\mod.ini\swap
//   set <name> <value>  SetVars
//   ping
//   quit

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <sstream>
#include "../../../../../DirectX11/desktop/Protocol.h"
#include "json.h"

using namespace DesktopProtocol;

static FILE *log_file;
static CRITICAL_SECTION print_lock;
static HANDLE pipe = INVALID_HANDLE_VALUE;
static HANDLE write_event;
static volatile LONG next_id = 1;
static volatile double pick_sent;
static volatile bool ready;

static void print(const char *fmt, ...)
{
	va_list ap, copy;
	va_start(ap, fmt);
	va_copy(copy, ap);
	std::string buf(_vscprintf(fmt, copy) + 1, '\0');
	va_end(copy);
	vsnprintf(&buf[0], buf.size(), fmt, ap);
	va_end(ap);
	SYSTEMTIME t;
	GetLocalTime(&t);
	EnterCriticalSection(&print_lock);
	fputs(buf.c_str(), stdout);
	if (log_file) {
		fprintf(log_file, "%02d:%02d:%02d.%03d %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf.c_str());
		fflush(log_file);
	}
	LeaveCriticalSection(&print_lock);
}

static double now_ms()
{
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return c.QuadPart * 1000.0 / f.QuadPart;
}

static bool io(bool write, void *data, DWORD size, HANDLE event)
{
	OVERLAPPED ov = {};
	ov.hEvent = event;
	DWORD n = 0;
	BOOL ok = write ? WriteFile(pipe, data, size, NULL, &ov) : ReadFile(pipe, data, size, NULL, &ov);
	if (!ok && GetLastError() != ERROR_IO_PENDING)
		return false;
	return GetOverlappedResult(pipe, &ov, &n, TRUE) && n == size;
}

template <class M> static bool send(MessageType type, M m, uint32_t *id_out = NULL)
{
	uint32_t id = (uint32_t)InterlockedIncrement(&next_id);
	std::vector<uint8_t> frame;
	if (!EncodeFrame(type, id, m, &frame))
		return false;
	if (id_out)
		*id_out = id;
	return io(true, frame.data(), (DWORD)frame.size(), write_event);
}

static bool read_exact(void *out, DWORD size, HANDLE event)
{
	uint8_t *p = (uint8_t*)out;
	while (size) {
		OVERLAPPED ov = {};
		ov.hEvent = event;
		DWORD n = 0;
		if (!ReadFile(pipe, p, size, NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
			return false;
		if (!GetOverlappedResult(pipe, &ov, &n, TRUE) || !n)
			return false;
		p += n;
		size -= n;
	}
	return true;
}

static bool receive(FrameHeader *h, std::vector<uint8_t> *payload, HANDLE event)
{
	uint8_t header[HEADER_SIZE];
	if (!read_exact(header, HEADER_SIZE, event) || !ParseHeader(header, h))
		return false;
	payload->resize(h->payload_size);
	return !h->payload_size || read_exact(payload->data(), h->payload_size, event);
}

template <class M> static bool show(const char *name, const FrameHeader &h, const std::vector<uint8_t> &payload)
{
	M m;
	if (!DecodePayload(payload.data(), payload.size(), &m)) {
		print("MALFORMED %s of %u bytes\n", name, h.payload_size);
		return false;
	}
	print("{\"type\":\"%s\",\"requestId\":%u,\"message\":%s}\n", name, h.request_id, ToJson(m).c_str());
	return true;
}

static DWORD WINAPI reader(LPVOID)
{
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	FrameHeader h;
	std::vector<uint8_t> payload;
	while (receive(&h, &payload, event)) {
		bool ok = true;
		switch ((MessageType)h.type) {
		case MessageType::Pong: ok = show<Pong>("Pong", h, payload); break;
		case MessageType::Error: ok = show<Error>("Error", h, payload); break;
		case MessageType::VarsReply: ok = show<VarsReply>("VarsReply", h, payload); break;
		case MessageType::VarsApplied: ok = show<VarsApplied>("VarsApplied", h, payload); break;
		case MessageType::Event: ok = show<Event>("Event", h, payload); break;
		case MessageType::PickArmed: ok = show<PickArmed>("PickArmed", h, payload); break;
		case MessageType::PickFailed: ok = show<PickFailed>("PickFailed", h, payload); break;
		case MessageType::PickResult: {
			double sent = pick_sent;
			ok = show<PickResult>("PickResult", h, payload);
			if (sent)
				print("  %u bytes, %.1f ms after the PickArm\n", h.payload_size, now_ms() - sent);
			pick_sent = 0;
			break;
		}
		default:
			print("type 0x%04x, %u bytes\n", h.type, h.payload_size);
		}
		if (!ok)
			break;
	}
	print("connection ended (%lu)\n", GetLastError());
	CloseHandle(event);
	return 0;
}

static DWORD find_game()
{
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
	DWORD pid = 0;
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	do {
		if (!wcsncmp(fd.cFileName, L"nahida-migoto-", 14))
			pid = wcstoul(fd.cFileName + 14, NULL, 10);
	} while (!pid && FindNextFileW(find, &fd));
	FindClose(find);
	return pid;
}

static bool elevated()
{
	HANDLE token;
	TOKEN_ELEVATION e = {};
	DWORD n;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n);
	CloseHandle(token);
	return e.TokenIsElevated != 0;
}

static bool game_alive(DWORD pid)
{
	HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!p)
		return GetLastError() == ERROR_ACCESS_DENIED;
	bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
	CloseHandle(p);
	return alive;
}

// One connection, from opening the pipe to its end; false when the game is gone
static bool session(DWORD pid)
{
	wchar_t name[64];
	_snwprintf_s(name, _TRUNCATE, L"\\\\.\\pipe\\nahida-migoto-%lu", pid);
	DWORD error = 0;
	for (;;) {
		pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (pipe != INVALID_HANDLE_VALUE)
			break;
		if (GetLastError() != error) {
			error = GetLastError();
			print("waiting for the pipe of %lu (%lu)\n", pid, error);
		}
		if (!game_alive(pid)) {
			print("the game exited\n");
			return false;
		}
		Sleep(500);
	}
	ULONG server = 0;
	if (!GetNamedPipeServerProcessId(pipe, &server) || server != pid) {
		print("the pipe's server is %lu, not %lu: closing\n", server, pid);
		CloseHandle(pipe);
		return false;
	}

	Hello hello;
	hello.client = "desktop channel console client";
	hello.min_version = 1;
	hello.max_version = 1;
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	FrameHeader h;
	std::vector<uint8_t> payload;
	double start = now_ms();
	if (!send(MessageType::Hello, hello) || !receive(&h, &payload, event)) {
		print("no answer to Hello (%lu)\n", GetLastError());
		CloseHandle(event);
		CloseHandle(pipe);
		return true;
	}
	if (h.type == (uint16_t)MessageType::HelloReply) {
		show<HelloReply>("HelloReply", h, payload);
		print("  connected in %.1f ms\n", now_ms() - start);
		ready = true;
	} else {
		show<Error>("Error", h, payload);
	}
	CloseHandle(event);

	HANDLE thread = CreateThread(NULL, 0, reader, NULL, 0, NULL);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	ready = false;
	HANDLE old = pipe;
	pipe = INVALID_HANDLE_VALUE;
	CloseHandle(old);
	return true;
}

static DWORD WINAPI commands(LPVOID)
{
	char line[4096];
	while (fgets(line, sizeof(line), stdin)) {
		std::istringstream in(line);
		std::string cmd;
		in >> cmd;
		if (!ready && cmd != "quit") {
			print("not connected\n");
			continue;
		}
		bool ok = true;
		if (cmd.empty() || cmd == "pick") {
			PickArm arm;
			float x, y;
			if (in >> x >> y) {
				arm.cursor = 1;
				arm.x = x;
				arm.y = y;
			}
			pick_sent = now_ms();
			ok = send(MessageType::PickArm, arm);
		} else if (cmd == "get") {
			GetVars get;
			std::string n;
			while (in >> n)
				get.names.push_back(n);
			ok = send(MessageType::GetVars, get);
		} else if (cmd == "set") {
			SetVars set;
			Assign a;
			if (in >> a.name >> a.value)
				set.vars.push_back(a);
			ok = send(MessageType::SetVars, set);
		} else if (cmd == "ping") {
			Ping ping;
			ping.nonce = GetTickCount64();
			ok = send(MessageType::Ping, ping);
		} else if (cmd == "quit") {
			ExitProcess(0);
		} else {
			print("commands: pick [x y], get <name> ..., set <name> <value>, ping, quit\n");
		}
		if (!ok)
			print("send failed (%lu)\n", GetLastError());
	}
	return 0;
}

int main(int argc, char **argv)
{
	DWORD pid = 0;
	std::string log_path;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--log") && i + 1 < argc)
			log_path = argv[++i];
		else
			pid = strtoul(argv[i], NULL, 10);
	}
	if (log_path.empty()) {
		char exe[MAX_PATH];
		GetModuleFileNameA(NULL, exe, MAX_PATH);
		log_path = exe;
		log_path = log_path.substr(0, log_path.find_last_of('\\') + 1) + "client.log";
	}
	InitializeCriticalSection(&print_lock);
	log_file = fopen(log_path.c_str(), "a");
	write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	print("client %s elevated, log %s\n", elevated() ? "is" : "is not", log_path.c_str());

	while (!pid) {
		pid = find_game();
		if (!pid)
			Sleep(500);
	}
	print("game pid %lu\n", pid);
	CreateThread(NULL, 0, commands, NULL, 0, NULL);
	while (session(pid))
		Sleep(200);
	if (log_file)
		fclose(log_file);
	return 0;
}
