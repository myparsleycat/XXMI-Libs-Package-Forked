// Checks the desktop channel (docs/desktop-channel.md) without a game. Loads
// the d3d11.dll of a run folder, writes Mods\channel\mod.ini there with
// global variables, and presents a cleared WARP swap chain every frame while a
// client thread connects to the pipe the DLL serves. The client checks the
// handshake, a refused second client, Ping, GetVars and SetVars, the refusal
// of PickArm, then:
//
//   reload order  a SetVars carried out at the Present of the first
//                 configuration reload; RELOADED must leave before its
//                 VarsApplied
//   reconnect     disconnects during large variable requests; their responses
//                 must not reach the next connection
//   backpressure  512 GetVars and SetVars requests interleaved with unknown
//                 frames; each gets one matching reply or BUSY, and only
//                 acknowledged SetVars change variables
//   limits        responses of exactly 4 MiB and one byte over, which is
//                 rejected with the request ID and changes no variable
//
//   channel.exe <run folder> [idle <seconds>]
//
// With idle it presents for that long without its client, for client.exe to
// connect to. Exits with 2 when a check fails, 1 when the DLL or D3D11 fails.

#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../../../common/window.h"
#include "../../../../../DirectX11/desktop/Protocol.h"

#pragma comment(lib, "user32.lib")

using namespace DesktopProtocol;

typedef HRESULT (WINAPI *CreateFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

#define CHECK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { printf("FAILED %s = 0x%08x\n", #call, (unsigned)hr_); exit(1); } } while (0)

static const UINT WIDTH = 320, HEIGHT = 180;
static const char *TEST_VARIABLE = "$\\Mods\\channel\\mod.ini\\test";
static volatile LONG failures;
static volatile bool client_done;
static std::string run_folder;
// config_initialization_delay of the run folder's d3dx.ini
static DWORD reload_delay_ms;
static DWORD launch_tick, first_present_ms;
// After the first Present, which starts the channel, presents wait until the
// client lets them go on, so that the first configuration reload runs when
// the client chooses
static volatile LONG hold_present = 1, frames_presented;

static void expect(const char *what, bool ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		InterlockedIncrement(&failures);
}

static HANDLE open_pipe(DWORD *error)
{
	wchar_t name[64];
	_snwprintf_s(name, _TRUNCATE, L"\\\\.\\pipe\\nahida-migoto-%lu", GetCurrentProcessId());
	HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	*error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
	return h;
}

static bool write_all(HANDLE h, const std::vector<uint8_t> &data)
{
	DWORD n;
	return WriteFile(h, data.data(), (DWORD)data.size(), &n, NULL) && n == data.size();
}

template <class M> static bool send(HANDLE h, MessageType type, uint32_t id, M m)
{
	std::vector<uint8_t> frame;
	return EncodeFrame(type, id, m, &frame) && write_all(h, frame);
}

static bool read_exact(HANDLE h, void *out, DWORD size)
{
	uint8_t *p = (uint8_t*)out;
	while (size) {
		DWORD n = 0;
		if (!ReadFile(h, p, size, &n, NULL) || !n)
			return false;
		p += n;
		size -= n;
	}
	return true;
}

struct Received {
	FrameHeader header;
	std::vector<uint8_t> payload;
};

static bool receive(HANDLE h, Received *r)
{
	uint8_t header[HEADER_SIZE];
	if (!read_exact(h, header, HEADER_SIZE) || !ParseHeader(header, &r->header))
		return false;
	r->payload.resize(r->header.payload_size);
	return !r->header.payload_size || read_exact(h, r->payload.data(), r->header.payload_size);
}

// Skips events, which the server may send between any two replies
template <class M> static bool receive_as(HANDLE h, MessageType type, uint32_t id, M *m)
{
	Received r;
	do {
		if (!receive(h, &r))
			return false;
	} while (r.header.type == (uint16_t)MessageType::Event);
	return r.header.type == (uint16_t)type && r.header.request_id == id && DecodePayload(r.payload.data(), r.payload.size(), m);
}

static bool error_reply(HANDLE h, uint32_t id, const char *code)
{
	Error e;
	return receive_as(h, MessageType::Error, id, &e) && e.code == code;
}

template <class M> static void check_large_rejection(HANDLE h, MessageType type, uint32_t id, M &message)
{
	Ping ping;
	ping.nonce = id;
	bool sent = send(h, type, id, message) && send(h, MessageType::Ping, id + 1, ping);
	expect("the oversized-response request fits in a frame", sent);
	if (!sent)
		return;
	Received r;
	if (!receive(h, &r)) {
		expect("the oversized-response request is answered", false);
		return;
	}
	Error error;
	bool rejected = r.header.type == (uint16_t)MessageType::Error && r.header.request_id == id
		&& DecodePayload(r.payload.data(), r.payload.size(), &error) && error.code == "RESPONSE_TOO_LARGE";
	expect("the oversized response is rejected with the request ID", rejected);
	if (r.header.type != (uint16_t)MessageType::Pong && !receive(h, &r)) {
		expect("Ping after rejection is answered", false);
		return;
	}
	Pong pong;
	expect("the connection remains usable after rejection", r.header.type == (uint16_t)MessageType::Pong
		&& r.header.request_id == id + 1 && DecodePayload(r.payload.data(), r.payload.size(), &pong) && pong.nonce == id);
}

static void check_variable_limits(HANDLE h)
{
	GetVars get;
	get.names.assign(64, std::string(65535, 'a'));
	size_t excess = HEADER_SIZE + 2 + get.names.size() * (65535 + 7) - MAX_FRAME_SIZE;
	get.names.back().resize(65535 - excess);
	VarsReply reply;
	bool ok = send(h, MessageType::GetVars, 100, get) && receive_as(h, MessageType::VarsReply, 100, &reply);
	expect("GetVars accepts a response of exactly 4 MiB", ok && reply.values.size() == get.names.size()
		&& reply.values.back().name == get.names.back() && !reply.values.back().exists);
	get.names.back().push_back('a');
	check_large_rejection(h, MessageType::GetVars, 101, get);

	SetVars set;
	set.vars.push_back({ TEST_VARIABLE, 9 });
	for (const std::string &filler : get.names)
		set.vars.push_back({ filler, 0 });
	size_t size = HEADER_SIZE + 8 + 2;
	for (const Assign &a : set.vars)
		size += 7 + a.name.size();
	set.vars.back().name.resize(set.vars.back().name.size() - (size - MAX_FRAME_SIZE) + 1);
	check_large_rejection(h, MessageType::SetVars, 103, set);
	get.names = { TEST_VARIABLE };
	ok = send(h, MessageType::GetVars, 105, get) && receive_as(h, MessageType::VarsReply, 105, &reply);
	expect("rejected SetVars does not change any variable", ok && reply.values.size() == 1
		&& reply.values[0].exists && reply.values[0].value == 5);

	set.vars.back().name.pop_back();
	VarsApplied applied;
	ok = send(h, MessageType::SetVars, 106, set) && receive_as(h, MessageType::VarsApplied, 106, &applied);
	expect("SetVars accepts a response of exactly 4 MiB", ok && applied.values.size() == set.vars.size()
		&& applied.values[0].exists && applied.values[0].value == 9 && applied.values.back().name == set.vars.back().name);
}

static void check_reconnect(HANDLE *connection)
{
	Hello hello;
	hello.client = "reconnect harness";
	hello.min_version = hello.max_version = 1;
	GetVars large, barrier;
	large.names.assign(65535, "$\\missing-variable-xxxxxxxxxxxxxxxxxxxxxxxxx");
	barrier.names = { TEST_VARIABLE };
	bool isolated = true;
	for (unsigned trial = 0; trial < 12; trial++) {
		if (!send(*connection, MessageType::GetVars, 1000 + trial, large)) {
			expect("GetVars before reconnect is sent", false);
			return;
		}
		Sleep(10 + trial * 2);
		CloseHandle(*connection);
		DWORD error;
		for (unsigned retry = 0; retry < 500; retry++) {
			*connection = open_pipe(&error);
			if (*connection != INVALID_HANDLE_VALUE)
				break;
			Sleep(1);
		}
		HelloReply info;
		if (*connection == INVALID_HANDLE_VALUE || !send(*connection, MessageType::Hello, 1, hello)
				|| !receive_as(*connection, MessageType::HelloReply, 1, &info)
				|| !send(*connection, MessageType::GetVars, 2, barrier)) {
			expect("the new connection accepts requests", false);
			return;
		}
		Received received;
		bool answered = false;
		while (receive(*connection, &received)) {
			if (received.header.type == (uint16_t)MessageType::Event)
				continue;
			if (received.header.request_id >= 1000) {
				isolated = false;
				continue;
			}
			VarsReply reply;
			answered = received.header.type == (uint16_t)MessageType::VarsReply
				&& received.header.request_id == 2 && DecodePayload(received.payload.data(), received.payload.size(), &reply)
				&& reply.values.size() == 1 && reply.values[0].exists && reply.values[0].value == 5;
			break;
		}
		expect("the new connection receives its own variable response", answered);
	}
	expect("responses from disconnected clients never reach a new connection", isolated);
}

static std::string pressure_variable(unsigned index)
{
	return "$\\Mods\\channel\\mod.ini\\pressure" + std::to_string(index);
}

static void check_reply_backpressure(HANDLE h)
{
	const unsigned requests = 512;
	const uint32_t first_id = 2000;
	std::vector<uint8_t> bytes, frame;
	bytes.reserve(128 * 1024 * 1024 + 65536);
	for (unsigned i = 0; i < requests; i++) {
		bool encoded;
		if (i % 2) {
			SetVars set;
			set.vars.push_back({ pressure_variable(i / 2), (float)(i + 1) });
			encoded = EncodeFrame(MessageType::SetVars, first_id + i, set, &frame);
		} else {
			GetVars get;
			encoded = EncodeFrame(MessageType::GetVars, first_id + i, get, &frame);
		}
		if (!encoded) {
			expect("backpressure requests encode", false);
			return;
		}
		bytes.insert(bytes.end(), frame.begin(), frame.end());
		if (i % 4 == 3) {
			// Unknown frames keep reads ready while the render thread queues replies.
			size_t offset = bytes.size();
			bytes.resize(offset + 1024 * 1024);
			uint32_t length = 1024 * 1024 - 4;
			uint16_t type = 0x7ffe;
			memcpy(bytes.data() + offset, &length, 4);
			memcpy(bytes.data() + offset + 4, &type, 2);
		}
	}
	if (!write_all(h, bytes)) {
		expect("backpressure requests are sent", false);
		return;
	}
	std::vector<bool> seen(requests), applied(requests / 2);
	unsigned responses = 0, busy = 0;
	bool valid = true;
	DWORD until = GetTickCount() + 5000;
	while (responses < requests && (int)(until - GetTickCount()) > 0) {
		DWORD available = 0;
		if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL))
			break;
		if (available < HEADER_SIZE) {
			Sleep(1);
			continue;
		}
		Received r;
		if (!receive(h, &r))
			break;
		if (r.header.type == (uint16_t)MessageType::Event)
			continue;
		if (r.header.request_id < first_id || r.header.request_id >= first_id + requests) {
			valid = false;
			continue;
		}
		unsigned i = r.header.request_id - first_id;
		if (seen[i]) {
			valid = false;
			continue;
		}
		seen[i] = true;
		responses++;
		if (r.header.type == (uint16_t)MessageType::Error) {
			Error error;
			valid &= DecodePayload(r.payload.data(), r.payload.size(), &error) && error.code == "BUSY";
			busy++;
		} else if (i % 2) {
			VarsApplied reply;
			applied[i / 2] = true;
			valid &= r.header.type == (uint16_t)MessageType::VarsApplied
				&& DecodePayload(r.payload.data(), r.payload.size(), &reply) && reply.values.size() == 1
				&& reply.values[0].name == pressure_variable(i / 2) && reply.values[0].exists
				&& reply.values[0].value == (float)(i + 1);
		} else {
			VarsReply reply;
			valid &= r.header.type == (uint16_t)MessageType::VarsReply
				&& DecodePayload(r.payload.data(), r.payload.size(), &reply) && reply.values.empty();
		}
	}
	printf("  backpressure: %u responses, %u BUSY, %u missing\n", responses, busy, requests - responses);
	expect("every backpressure request receives one matching response", responses == requests && valid);

	GetVars get;
	for (unsigned i = 0; i < requests / 2; i++)
		get.names.push_back(pressure_variable(i));
	VarsReply reply;
	bool ok = send(h, MessageType::GetVars, first_id + requests, get)
		&& receive_as(h, MessageType::VarsReply, first_id + requests, &reply)
		&& reply.values.size() == get.names.size();
	expect("requests succeed after the reply queue drains", ok);
	for (unsigned i = 0; ok && i < reply.values.size(); i++)
		ok = reply.values[i].name == get.names[i] && reply.values[i].exists
			&& reply.values[i].value == (applied[i] ? (float)(i * 2 + 2) : 0);
	expect("only acknowledged SetVars requests change variables under backpressure", ok);
}

// The pipe thread answers each Ping with a Pong of this size, written as it
// reads. Once the Pongs waiting in the pipe stop growing short of one per Ping
// sent, the pipe thread is waiting in a write for the client to read.
static bool pipe_thread_held(HANDLE h, unsigned pings)
{
	const DWORD PONG_SIZE = HEADER_SIZE + 8;
	DWORD last = MAXDWORD;
	int stable = 0;

	for (int i = 0; i < 500 && stable < 5; i++) {
		DWORD available = 0;
		if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL) || available == pings * PONG_SIZE)
			return false;
		stable = available == last ? stable + 1 : 0;
		last = available;
		Sleep(10);
	}
	return stable >= 5;
}

// The first configuration reload and a SetVars carried out at the same
// Present, while the pipe thread is held writing Pongs the client has not read
// yet, so that RELOADED and VarsApplied are both queued before it sends either
static void check_reload_order(HANDLE h)
{
	while (!frames_presented)
		Sleep(1);
	expect("config_initialization_delay leaves time for the first Present", first_present_ms < reload_delay_ms);
	while (GetTickCount() - launch_tick < reload_delay_ms + 1000)
		Sleep(10);

	SetVars set;
	set.vars = { { "$\\global\\missing", 1 } };
	bool ok = send(h, MessageType::SetVars, 7, set), held = false;
	unsigned pings = 0;
	while (ok && !held && pings < (1u << 20)) {
		std::vector<uint8_t> batch, frame;
		for (unsigned i = 0; ok && i < 64; i++, pings++) {
			Ping ping;
			ping.nonce = pings;
			ok = EncodeFrame(MessageType::Ping, 8, ping, &frame);
			batch.insert(batch.end(), frame.begin(), frame.end());
		}
		ok = ok && write_all(h, batch);
		held = ok && pipe_thread_held(h, pings);
	}
	expect("the pipe thread is held writing Pongs", held);

	LONG frames = frames_presented;
	InterlockedExchange(&hold_present, 0);
	while (frames_presented == frames)
		Sleep(1);

	int event_at = -1, applied_at = -1;
	Event e;
	VarsApplied applied;
	for (unsigned i = 0; ok && i < pings + 2; i++) {
		Received r;
		ok = receive(h, &r);
		if (ok && r.header.type == (uint16_t)MessageType::Event)
			event_at = DecodePayload(r.payload.data(), r.payload.size(), &e) && e.kind == EVENT_RELOADED ? i : -1;
		else if (ok && r.header.type == (uint16_t)MessageType::VarsApplied && r.header.request_id == 7)
			applied_at = DecodePayload(r.payload.data(), r.payload.size(), &applied) ? i : -1;
	}
	expect("the reload ran at the Present that carried out the SetVars",
		ok && event_at >= 0 && applied_at >= 0 && e.frame == applied.frame);
	expect("RELOADED leaves before the VarsApplied of the same Present", ok && event_at >= 0 && applied_at > event_at);
}

static DWORD WINAPI client(LPVOID)
{
	HANDLE h = INVALID_HANDLE_VALUE;
	DWORD error = 0;

	for (int i = 0; i < 500 && h == INVALID_HANDLE_VALUE; i++) {
		h = open_pipe(&error);
		if (h == INVALID_HANDLE_VALUE)
			Sleep(10);
	}
	expect("the pipe is there after the first Present", h != INVALID_HANDLE_VALUE);
	if (h == INVALID_HANDLE_VALUE) {
		client_done = true;
		return 0;
	}
	ULONG server = 0;
	expect("the pipe's server is this process", GetNamedPipeServerProcessId(h, &server) && server == GetCurrentProcessId());

	Hello hello;
	hello.client = "channel harness";
	hello.min_version = 1;
	hello.max_version = 1;
	HelloReply info;
	bool ok = send(h, MessageType::Hello, 1, hello) && receive_as(h, MessageType::HelloReply, 1, &info);
	expect("HelloReply", ok);
	expect("HelloReply: version 1, this process, D3D11, VARS only", info.version == 1 && info.pid == GetCurrentProcessId()
		&& info.render_api == RENDER_D3D11 && info.capabilities == CAPABILITY_VARS);
	expect("HelloReply: the importer folder is the run folder", !_stricmp(info.importer_folder.c_str(), run_folder.c_str()));
	printf("  DLL version %s, exe %s\n", info.dll_version.c_str(), info.exe_path.c_str());

	DWORD second_error;
	HANDLE second = open_pipe(&second_error);
	expect("a second client is refused", second == INVALID_HANDLE_VALUE && second_error == ERROR_PIPE_BUSY);
	if (second != INVALID_HANDLE_VALUE)
		CloseHandle(second);

	Ping ping;
	ping.nonce = 0x1122334455667788ull;
	Pong pong;
	expect("Ping is answered with its nonce", send(h, MessageType::Ping, 2, ping) && receive_as(h, MessageType::Pong, 2, &pong)
		&& pong.nonce == ping.nonce);

	check_reload_order(h);

	GetVars get;
	get.names = { TEST_VARIABLE, "$\\global\\missing" };
	VarsReply vars;
	ok = send(h, MessageType::GetVars, 3, get) && receive_as(h, MessageType::VarsReply, 3, &vars);
	expect("GetVars: the mod's variable and a missing one", ok && vars.values.size() == 2 && vars.values[0].exists
		&& vars.values[0].value == 2 && !vars.values[1].exists);
	SetVars set;
	set.vars = { { TEST_VARIABLE, 5 }, { "$\\global\\missing", 1 } };
	VarsApplied applied;
	ok = send(h, MessageType::SetVars, 4, set) && receive_as(h, MessageType::VarsApplied, 4, &applied);
	expect("SetVars: applied, the missing one not created", ok && applied.values.size() == 2 && applied.values[0].exists
		&& applied.values[0].value == 5 && !applied.values[1].exists && applied.frame > 0);
	ok = send(h, MessageType::GetVars, 5, get) && receive_as(h, MessageType::VarsReply, 5, &vars);
	expect("GetVars after SetVars", ok && vars.values.size() == 2 && vars.values[0].value == 5);

	PickArm arm;
	arm.cursor = 1;
	arm.x = arm.y = 0.5f;
	expect("PickArm is refused as UNSUPPORTED", send(h, MessageType::PickArm, 6, arm) && error_reply(h, 6, "UNSUPPORTED"));

	check_reconnect(&h);
	check_reply_backpressure(h);
	check_variable_limits(h);

	CloseHandle(h);
	client_done = true;
	return 0;
}

static void write_mod_ini(const std::string &folder)
{
	std::string dir = folder + "\\Mods\\channel";
	CreateDirectoryA((folder + "\\Mods").c_str(), NULL);
	CreateDirectoryA(dir.c_str(), NULL);
	FILE *f = fopen((dir + "\\mod.ini").c_str(), "w");
	if (!f) {
		printf("cannot write %s\\mod.ini\n", dir.c_str());
		exit(1);
	}
	fprintf(f, "[Constants]\nglobal $test = 2\n");
	for (unsigned i = 0; i < 256; i++)
		fprintf(f, "global $pressure%u = 0\n", i);
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("channel.exe <run folder> [idle <seconds>]\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	char full[MAX_PATH];
	GetFullPathNameA(argv[1], MAX_PATH, full, NULL);
	run_folder = full;
	while (!run_folder.empty() && run_folder.back() == '\\')
		run_folder.pop_back();
	DWORD idle_ms = argc > 3 && !strcmp(argv[2], "idle") ? (DWORD)(atof(argv[3]) * 1000) : 0;
	write_mod_ini(run_folder);
	reload_delay_ms = GetPrivateProfileIntA("System", "config_initialization_delay", 0,
			(run_folder + "\\d3dx.ini").c_str()) * 1000;
	if (idle_ms)
		hold_present = 0;

	launch_tick = GetTickCount();
	HMODULE lib = LoadLibraryA((run_folder + "\\d3d11.dll").c_str());
	if (!lib) {
		printf("cannot load the DLL: %lu\n", GetLastError());
		return 1;
	}
	CreateFn create = (CreateFn)GetProcAddress(lib, "D3D11CreateDeviceAndSwapChain");

	WNDCLASSW wc = {};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"channel-harness";
	RegisterClassW(&wc);
	HWND window = CreateWindowW(wc.lpszClassName, L"channel-harness", WS_OVERLAPPEDWINDOW, 40, 40,
			WIDTH, HEIGHT, NULL, NULL, wc.hInstance, NULL);
	show_harness_window(window);

	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferDesc.Width = WIDTH;
	scd.BufferDesc.Height = HEIGHT;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 2;
	scd.OutputWindow = window;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	IDXGISwapChain *swap;
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;
	CHECK(create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &level, 1, D3D11_SDK_VERSION, &scd, &swap, &dev, NULL, &ctx));
	ID3D11Texture2D *back;
	ID3D11RenderTargetView *rtv;
	CHECK(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back));
	CHECK(dev->CreateRenderTargetView(back, NULL, &rtv));
	back->Release();

	if (!idle_ms)
		CloseHandle(CreateThread(NULL, 0, client, NULL, 0, NULL));
	DWORD start = GetTickCount();
	const float color[4] = { 0.1f, 0.2f, 0.3f, 1 };
	for (;;) {
		if (idle_ms ? GetTickCount() - start >= idle_ms : client_done)
			break;
		if (!idle_ms && GetTickCount() - start > 120000) {
			expect("the client finishes within two minutes", false);
			break;
		}
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);
		if (hold_present && frames_presented) {
			Sleep(1);
			continue;
		}
		ctx->OMSetRenderTargets(1, &rtv, NULL);
		ctx->ClearRenderTargetView(rtv, color);
		swap->Present(0, 0);
		if (!frames_presented)
			first_present_ms = GetTickCount() - launch_tick;
		InterlockedIncrement(&frames_presented);
		Sleep(5);
	}
	rtv->Release();
	ctx->Release();
	swap->Release();
	dev->Release();
	DestroyWindow(window);

	printf("%ld failures\n", failures);
	return failures ? 2 : 0;
}
