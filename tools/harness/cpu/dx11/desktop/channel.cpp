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
//   overlay       a shared texture of a second WARP device drawn over the
//                 presented frame, a held keyed mutex that must not stall
//                 the game, a shared memory section, input capture through
//                 the window's messages, and a reconnect that drops it all
//
//   channel.exe <run folder> [idle <seconds>]
//
// With idle it presents for that long without its client, for client.exe to
// connect to. Exits with 2 when a check fails, 1 when the DLL or D3D11 fails.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>
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
static HWND harness_window;
static bool srgb_back_buffer;
// In srgb mode the render loop presents with a geometry shader bound that
// emits nothing, as a game may leave one bound after its last draw
static ID3D11GeometryShader *present_gs;
static volatile LONG present_gs_lost;
// Messages that reached the harness's own window procedure
static volatile LONG game_mouse_moves, game_keys, game_key_ups, game_button_ups;
// The render loop presents with an occlusion predicate set that skips
// rendering, as a game may leave it
static volatile LONG present_predicated;
// The render loop calls SetCursor(NULL) as a game that hides the cursor does,
// and records the cursor of the window's thread after every Present
static volatile LONG hide_window_cursor;
// The render loop calls TranslateMessage for every posted key press, as a
// game's message loop does
static volatile LONG translate_presses;
static HCURSOR volatile window_cursor;
// A pixel of the back buffer as presented, which the render loop reads when
// the client asks for it
static volatile LONG readback_x, readback_y, readback_pending;
static uint32_t readback_pixel;

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

// What the server sends on its own, between any two replies
static bool unsolicited(const Received &r)
{
	return r.header.request_id == 0 && (r.header.type == (uint16_t)MessageType::Event
		|| r.header.type == (uint16_t)MessageType::OverlaySurface || r.header.type == (uint16_t)MessageType::OverlayInput);
}

template <class M> static bool receive_as(HANDLE h, MessageType type, uint32_t id, M *m)
{
	Received r;
	do {
		if (!receive(h, &r))
			return false;
	} while (unsolicited(r));
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
			if (unsolicited(received))
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
		if (unsolicited(r))
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

// Sends Pings without reading until the pipe thread is held writing a Pong.
// The client must read *pings Pongs before it writes again.
static bool hold_pipe_thread(HANDLE h, uint32_t id, unsigned *pings)
{
	bool ok = true, held = false;
	*pings = 0;
	while (ok && !held && *pings < (1u << 20)) {
		std::vector<uint8_t> batch, frame;
		for (unsigned i = 0; ok && i < 64; i++, (*pings)++) {
			Ping ping;
			ping.nonce = *pings;
			ok = EncodeFrame(MessageType::Ping, id, ping, &frame);
			batch.insert(batch.end(), frame.begin(), frame.end());
		}
		ok = ok && write_all(h, batch);
		held = ok && pipe_thread_held(h, *pings);
	}
	return held;
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
	bool ok = send(h, MessageType::SetVars, 7, set);
	unsigned pings = 0;
	ok = ok && hold_pipe_thread(h, 8, &pings);
	expect("the pipe thread is held writing Pongs", ok);

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

static LRESULT CALLBACK harness_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_MOUSEMOVE)
		InterlockedIncrement(&game_mouse_moves);
	else if (msg == WM_KEYDOWN || msg == WM_CHAR)
		InterlockedIncrement(&game_keys);
	else if (msg == WM_KEYUP)
		InterlockedIncrement(&game_key_ups);
	else if (msg == WM_LBUTTONUP)
		InterlockedIncrement(&game_button_ups);
	else if (msg == WM_APP) {
		// Posted key messages leave the key state as it is: the checks set it
		// on the window's thread, in order with those messages
		BYTE state[256];
		GetKeyboardState(state);
		state[wp & 0xff] = lp ? 0x80 : 0;
		SetKeyboardState(state);
		return 0;
	} else if (msg == WM_APP + 1) {
		// Registers the mouse and keyboard as a game that reads only raw
		// input does, or removes them
		RAWINPUTDEVICE devices[2] = {
			{ 0x01, 0x02, (DWORD)(wp ? RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE : RIDEV_REMOVE), wp ? hwnd : NULL },
			{ 0x01, 0x06, (DWORD)(wp ? RIDEV_NOLEGACY | RIDEV_NOHOTKEYS : RIDEV_REMOVE), wp ? hwnd : NULL },
		};
		return RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
	} else if (msg == WM_APP + 2) {
		// The registered flags of the mouse (low word) and keyboard (high word)
		RAWINPUTDEVICE devices[8];
		UINT count = ARRAYSIZE(devices);
		UINT got = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
		LRESULT flags = 0;
		for (UINT i = 0; got != (UINT)-1 && i < got; i++) {
			if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x02)
				flags |= devices[i].dwFlags & 0xffff;
			else if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x06)
				flags |= (devices[i].dwFlags & 0xffff) << 16;
		}
		return flags;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void wait_presents(LONG n)
{
	LONG until = frames_presented + n;
	while (frames_presented < until)
		Sleep(1);
}

static uint32_t read_pixel(int x, int y)
{
	readback_x = x;
	readback_y = y;
	InterlockedExchange(&readback_pending, 1);
	while (readback_pending)
		Sleep(1);
	return readback_pixel;
}

static bool near_rgb(uint32_t pixel, int r, int g, int b)
{
	int got[3] = { (int)(pixel & 0xff), (int)((pixel >> 8) & 0xff), (int)((pixel >> 16) & 0xff) };
	int want[3] = { r, g, b };
	for (int i = 0; i < 3; i++) {
		if (abs(got[i] - want[i]) > 3) {
			printf("  pixel %d,%d,%d, expected %d,%d,%d\n", got[0], got[1], got[2], r, g, b);
			return false;
		}
	}
	return true;
}

static bool wait_count(volatile LONG *count, LONG above)
{
	for (int i = 0; i < 2000 && *count <= above; i++)
		Sleep(1);
	return *count > above;
}

static bool receive_input(HANDLE h, OverlayInput *input)
{
	Received r;
	do {
		if (!receive(h, &r))
			return false;
	} while (r.header.type != (uint16_t)MessageType::OverlayInput && unsolicited(r));
	return r.header.type == (uint16_t)MessageType::OverlayInput && r.header.request_id == 0
		&& DecodePayload(r.payload.data(), r.payload.size(), input);
}

static bool capture(HANDLE h, bool on, uint32_t id)
{
	OverlayCapture request;
	request.capture = on;
	request.cursor = CURSOR_HAND;
	OverlayCaptured reply;
	return send(h, MessageType::OverlayCapture, id, request) && receive_as(h, MessageType::OverlayCaptured, id, &reply);
}

// Reads the Pongs of hold_pipe_thread() and the input queued meanwhile, then
// ends the capture with request off_id and returns once it is answered
static bool read_backlog(HANDLE h, uint32_t ping_id, unsigned pings, uint32_t off_id, std::vector<OverlayInput> *in)
{
	unsigned pongs = 0;
	for (;;) {
		Received r;
		OverlayInput input;
		if (!receive(h, &r))
			return false;
		if (r.header.type == (uint16_t)MessageType::Pong && r.header.request_id == ping_id && ++pongs == pings) {
			OverlayCapture off;
			if (!send(h, MessageType::OverlayCapture, off_id, off))
				return false;
		} else if (r.header.type == (uint16_t)MessageType::OverlayInput) {
			if (!DecodePayload(r.payload.data(), r.payload.size(), &input))
				return false;
			in->push_back(input);
		} else if (r.header.type == (uint16_t)MessageType::OverlayCaptured && r.header.request_id == off_id) {
			return true;
		}
	}
}

// More input than the server queues while the client does not read: moves
// give way, and the release and the end of the capture still arrive. When
// only releases are left, the capture ends.
static void check_input_backlog(HANDLE h)
{
	const int MOVES = 3000;
	unsigned pings = 0;
	bool ok = capture(h, true, 330) && hold_pipe_thread(h, 331, &pings);
	expect("the pipe thread is held before a capture's input", ok);
	LONG moves = game_mouse_moves;
	PostMessageW(harness_window, WM_KEYDOWN, 'A', 0x001e0001);
	for (int i = 0; i < MOVES; i++)
		PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(i % 300, i / 300));
	PostMessageW(harness_window, WM_KEYUP, 'A', 0xc01e0001);
	PostMessageW(harness_window, WM_KILLFOCUS, 0, 0);
	// The window handled all of the above once a move reaches the game
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
	ok = ok && wait_count(&game_mouse_moves, moves);

	std::vector<OverlayInput> in;
	ok = ok && read_backlog(h, 331, pings, 332, &in);
	size_t n = in.size();
	int last = MOVES - 1;
	expect("a full input queue keeps the press, release and end of the capture",
		ok && n >= 4 && in[0].kind == INPUT_KEY && in[0].down && in[n - 2].kind == INPUT_KEY && in[n - 2].vk == 'A'
		&& !in[n - 2].down && in[n - 1].kind == INPUT_CAPTURE_ENDED);
	// 1024 and capture ended
	expect("a full input queue drops older moves and keeps the last position", ok && n >= 4 && n <= 1025
		&& in[n - 3].kind == INPUT_MOVE && in[n - 3].x == last % 300 && in[n - 3].y == last / 300);

	// Releases alone fill the queue, and the window keeps the focus
	const int PAIRS = 1100;
	OverlayInput z;
	ok = capture(h, true, 340);
	PostMessageW(harness_window, WM_KEYDOWN, 'Z', 0x002c0001);
	ok = ok && receive_input(h, &z) && z.kind == INPUT_KEY && z.vk == 'Z' && z.down && hold_pipe_thread(h, 341, &pings);
	expect("the pipe thread is held while the desktop holds a key", ok);
	moves = game_mouse_moves;
	for (int i = 0; i < PAIRS; i++) {
		PostMessageW(harness_window, WM_KEYDOWN, 'A', 0x001e0001);
		PostMessageW(harness_window, WM_KEYUP, 'A', 0xc01e0001);
	}
	PostMessageW(harness_window, WM_KEYUP, 'Z', 0xc02c0001);
	// The render loop handles the posted messages before its next Present,
	// which ends the capture
	wait_presents(3);
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
	expect("a release lost to a full queue ends the capture", ok && wait_count(&game_mouse_moves, moves));
	in.clear();
	ok = ok && read_backlog(h, 341, pings, 342, &in);
	n = in.size();
	expect("the client is told the capture ended after the lost release, with no input after it",
		ok && n && in[n - 1].kind == INPUT_CAPTURE_ENDED
		&& std::count_if(in.begin(), in.end(), [](const OverlayInput &i) { return i.kind == INPUT_CAPTURE_ENDED; }) == 1);
}

static void check_overlay(HANDLE *connection)
{
	const UINT W = 128, H = 96;
	// The clear color of the render loop and the overlay's pixels as UNORM
	const int CLEAR[3] = { 26, 51, 77 };
	const uint32_t RED = 0xff0000ff, HALF_WHITE = 0x80808080, WHITE = 0xffffffff;
	HANDLE h = *connection;

	// The desktop's side, on a device of its own
	wchar_t path[MAX_PATH];
	GetSystemDirectoryW(path, MAX_PATH);
	wcscat_s(path, L"\\d3d11.dll");
	PFN_D3D11_CREATE_DEVICE create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(LoadLibraryW(path),
		"D3D11CreateDevice");
	ID3D11Device *dev = NULL;
	ID3D11DeviceContext *ctx = NULL;
	if (!create_device || FAILED(create_device(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev,
			NULL, &ctx))) {
		expect("a second WARP device for the desktop side", false);
		return;
	}
	std::vector<uint32_t> pixels(W * H);
	for (UINT i = 0; i < W * H; i++)
		pixels[i] = i % W < W / 2 ? RED : HALF_WHITE;
	D3D11_TEXTURE2D_DESC desc = { W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX };
	D3D11_SUBRESOURCE_DATA data = { pixels.data(), W * 4, 0 };
	ID3D11Texture2D *shared;
	IDXGIResource *resource;
	IDXGIKeyedMutex *mutex;
	ID3D11RenderTargetView *rtv;
	HANDLE handle;
	CHECK(dev->CreateTexture2D(&desc, &data, &shared));
	CHECK(shared->QueryInterface(__uuidof(IDXGIResource), (void**)&resource));
	CHECK(resource->GetSharedHandle(&handle));
	CHECK(shared->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&mutex));
	CHECK(dev->CreateRenderTargetView(shared, NULL, &rtv));

	OverlaySetTexture set;
	set.kind = OVERLAY_KMT;
	set.handle = (uint32_t)(uintptr_t)handle;
	set.width = W;
	set.height = H;
	OverlayTextureSet set_reply;
	bool ok = send(h, MessageType::OverlaySetTexture, 300, set)
		&& receive_as(h, MessageType::OverlayTextureSet, 300, &set_reply);
	expect("OverlaySetTexture opens a shared texture of the game's adapter", ok);
	OverlayFrame frame;
	frame.seq = 1;
	send(h, MessageType::OverlayFrame, 301, frame);
	wait_presents(3);
	expect("the overlay is drawn over the presented frame", near_rgb(read_pixel(40, 80), 255, 0, 0));
	int blended[3];
	for (int i = 0; i < 3; i++)
		blended[i] = 128 + (CLEAR[i] * 127 + 127) / 255;
	expect("the overlay blends with premultiplied alpha",
		near_rgb(read_pixel(100, 80), blended[0], blended[1], blended[2]));
	expect("outside the overlay the frame is unchanged", near_rgb(read_pixel(200, 150), CLEAR[0], CLEAR[1], CLEAR[2]));

	const float green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
	mutex->AcquireSync(0, INFINITE);
	ctx->ClearRenderTargetView(rtv, green);
	ctx->Flush();
	mutex->ReleaseSync(0);
	frame.seq = 2;
	send(h, MessageType::OverlayFrame, 302, frame);
	wait_presents(3);
	expect("OverlayFrame brings a new picture", near_rgb(read_pixel(40, 80), 0, 255, 0));

	mutex->AcquireSync(0, INFINITE);
	ctx->ClearRenderTargetView(rtv, blue);
	ctx->Flush();
	frame.seq = 3;
	send(h, MessageType::OverlayFrame, 303, frame);
	wait_presents(3);
	expect("while the desktop holds the key, the game presents the previous picture",
		near_rgb(read_pixel(40, 80), 0, 255, 0));
	mutex->ReleaseSync(0);
	wait_presents(3);
	expect("the held frame is copied once the key is free", near_rgb(read_pixel(40, 80), 0, 0, 255));

	InterlockedExchange(&present_predicated, 1);
	wait_presents(3);
	expect("the overlay is drawn under a predicate the game left set", near_rgb(read_pixel(40, 80), 0, 0, 255));
	InterlockedExchange(&present_predicated, 0);

	// Input capture. W goes down to the game before the capture and the
	// game's cursor is hidden.
	LONG keys = game_keys;
	PostMessageW(harness_window, WM_KEYDOWN, 'W', 0x00110001);
	InterlockedExchange(&hide_window_cursor, 1);
	expect("a key reaches the game before the capture", wait_count(&game_keys, keys));
	wait_presents(2);
	LONG moves = game_mouse_moves, ups = game_key_ups;
	keys = game_keys;
	expect("OverlayCapture is answered", capture(h, true, 304));
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(5, 6));
	PostMessageW(harness_window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(7, 8));
	PostMessageW(harness_window, WM_KEYDOWN, 'A', 0x001e0001);
	PostMessageW(harness_window, WM_CHAR, 0xd83d, 0x001e0001);
	PostMessageW(harness_window, WM_CHAR, 0xde00, 0x001e0001);
	OverlayInput in[4];
	ok = true;
	for (int i = 0; ok && i < 4; i++)
		ok = receive_input(h, &in[i]);
	expect("captured mouse input is forwarded", ok && in[0].kind == INPUT_MOVE && in[0].x == 5 && in[0].y == 6
		&& in[1].kind == INPUT_BUTTON && in[1].button == BUTTON_LEFT && in[1].down && in[1].x == 7 && in[1].y == 8);
	expect("captured keys and characters are forwarded", ok && in[2].kind == INPUT_KEY && in[2].vk == 'A'
		&& in[2].scan == 0x1e && in[2].down && in[3].kind == INPUT_CHAR && in[3].rune == 0x1f600);
	expect("captured input does not reach the game", game_mouse_moves == moves && game_keys == keys);

	PostMessageW(harness_window, WM_KEYDOWN, 'W', 0x40110001);
	PostMessageW(harness_window, WM_KEYUP, 'W', 0xc0110001);
	// After the character TranslateMessage queued for the repeat
	wait_count(&game_key_ups, ups);
	PostMessageW(harness_window, WM_KEYUP, 'A', 0xc01e0001);
	OverlayInput up;
	ok = receive_input(h, &up);
	expect("a key held since before the capture is released to the game, and its repeat and character reach neither side",
		ok && up.kind == INPUT_KEY && up.vk == 'A' && !up.down && game_key_ups == ups + 1 && game_keys == keys);

	// Left and right Ctrl share VK_CONTROL, and each release belongs to its own press
	PostMessageW(harness_window, WM_KEYDOWN, VK_CONTROL, 0x001d0001);
	PostMessageW(harness_window, WM_KEYDOWN, VK_CONTROL, 0x011d0001);
	PostMessageW(harness_window, WM_KEYUP, VK_CONTROL, 0xc01d0001);
	PostMessageW(harness_window, WM_KEYUP, VK_CONTROL, 0xc11d0001);
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(3, 4));
	OverlayInput ctrl[5];
	ok = true;
	for (int i = 0; ok && i < 5; i++)
		ok = receive_input(h, &ctrl[i]);
	expect("the releases of left and right Ctrl pressed during the capture go to the desktop",
		ok && ctrl[0].kind == INPUT_KEY && ctrl[0].down && ctrl[0].scan == 0x1d
		&& ctrl[1].kind == INPUT_KEY && ctrl[1].down && ctrl[1].scan == 0xe01d
		&& ctrl[2].kind == INPUT_KEY && !ctrl[2].down && ctrl[2].scan == 0x1d
		&& ctrl[3].kind == INPUT_KEY && !ctrl[3].down && ctrl[3].scan == 0xe01d
		&& ctrl[4].kind == INPUT_MOVE && game_key_ups == ups + 1);

	// Both Shift keys are held and Windows sends no release for the left one
	// released first
	PostMessageW(harness_window, WM_APP, VK_LSHIFT, 1);
	PostMessageW(harness_window, WM_KEYDOWN, VK_SHIFT, 0x002a0001);
	PostMessageW(harness_window, WM_APP, VK_RSHIFT, 1);
	PostMessageW(harness_window, WM_KEYDOWN, VK_SHIFT, 0x00360001);
	PostMessageW(harness_window, WM_APP, VK_LSHIFT, 0);
	PostMessageW(harness_window, WM_APP, VK_RSHIFT, 0);
	PostMessageW(harness_window, WM_KEYUP, VK_SHIFT, 0xc0360001);
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(3, 4));
	OverlayInput shift[5];
	ok = true;
	for (int i = 0; ok && i < 5; i++)
		ok = receive_input(h, &shift[i]);
	expect("a Shift release Windows did not send goes to the desktop with the other Shift's release",
		ok && shift[0].kind == INPUT_KEY && shift[0].down && shift[0].scan == 0x2a
		&& shift[1].kind == INPUT_KEY && shift[1].down && shift[1].scan == 0x36
		&& shift[2].kind == INPUT_KEY && !shift[2].down && shift[2].scan == 0x2a
		&& shift[3].kind == INPUT_KEY && !shift[3].down && shift[3].scan == 0x36
		&& shift[4].kind == INPUT_MOVE && game_key_ups == ups + 1);
	wait_presents(2);
	expect("the capture sets its cursor", window_cursor == LoadCursor(NULL, IDC_HAND));

	PostMessageW(harness_window, WM_KILLFOCUS, 0, 0);
	OverlayInput ended;
	expect("losing focus ends the capture", receive_input(h, &ended) && ended.kind == INPUT_CAPTURE_ENDED);
	wait_presents(2);
	expect("the end of the capture restores the game's hidden cursor", window_cursor == NULL);
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
	expect("after the capture ended, input reaches the game", wait_count(&game_mouse_moves, moves));

	// A game that reads only raw input: the capture registers the mouse and
	// keyboard without RIDEV_NOLEGACY, keeping the keyboard's RIDEV_NOHOTKEYS,
	// and puts the game's registration back when it ends
	const LRESULT raw_game = (RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE) | (LRESULT)(RIDEV_NOLEGACY | RIDEV_NOHOTKEYS) << 16;
	ok = SendMessageW(harness_window, WM_APP + 1, 1, 0) && SendMessageW(harness_window, WM_APP + 2, 0, 0) == raw_game;
	expect("the game registers raw input with RIDEV_NOLEGACY", ok);
	ok = ok && capture(h, true, 312);
	LRESULT during = SendMessageW(harness_window, WM_APP + 2, 0, 0);
	expect("a capture receives the legacy messages of a game that reads only raw input",
		ok && during == (LRESULT)RIDEV_NOHOTKEYS << 16);
	ok = ok && capture(h, false, 313);
	LRESULT after = SendMessageW(harness_window, WM_APP + 2, 0, 0);
	expect("the end of the capture restores the game's raw input registration", ok && after == raw_game);
	SendMessageW(harness_window, WM_APP + 1, 0, 0);

	// The desktop ends the capture on a press it received
	ok = capture(h, true, 320);
	PostMessageW(harness_window, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
	PostMessageW(harness_window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(9, 9));
	PostMessageW(harness_window, WM_KEYDOWN, 'C', 0x002e0001);
	PostMessageW(harness_window, WM_KEYDOWN, 'D', 0x00200001);
	OverlayInput pressed[4];
	ok = ok && receive_input(h, &pressed[0]) && receive_input(h, &pressed[1]) && receive_input(h, &pressed[2])
		&& receive_input(h, &pressed[3]) && capture(h, false, 321);
	wait_presents(3);
	ups = game_key_ups;
	keys = game_keys;
	LONG button_ups = game_button_ups;
	moves = game_mouse_moves;
	InterlockedExchange(&translate_presses, 1);
	PostMessageW(harness_window, WM_KEYUP, VK_ESCAPE, 0xc0010001);
	PostMessageW(harness_window, WM_LBUTTONUP, 0, MAKELPARAM(9, 9));
	// The character of the repeat is queued behind the game's next press of
	// C, whose own character follows
	PostMessageW(harness_window, WM_KEYDOWN, 'C', 0x402e0001);
	PostMessageW(harness_window, WM_KEYUP, 'C', 0xc02e0001);
	PostMessageW(harness_window, WM_KEYDOWN, 'C', 0x002e0001);
	PostMessageW(harness_window, WM_KEYUP, 'C', 0xc02e0001);
	// D's release never arrived: the next press and its character are the game's
	PostMessageW(harness_window, WM_KEYDOWN, 'D', 0x00200001);
	PostMessageW(harness_window, WM_KEYUP, 'D', 0xc0200001);
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
	ok = ok && wait_count(&game_mouse_moves, moves);
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(2, 2));
	ok = ok && wait_count(&game_mouse_moves, moves + 1);
	InterlockedExchange(&translate_presses, 0);
	expect("after the capture ended, the releases, repeats and characters of presses the desktop received do not reach the game",
		ok && game_key_ups == ups + 2 && game_button_ups == button_ups && game_keys == keys + 4);

	// Both Shift keys go down during the capture, and after it the left one's
	// release is not sent
	ok = capture(h, true, 305);
	PostMessageW(harness_window, WM_APP, VK_LSHIFT, 1);
	PostMessageW(harness_window, WM_KEYDOWN, VK_SHIFT, 0x002a0001);
	PostMessageW(harness_window, WM_APP, VK_RSHIFT, 1);
	PostMessageW(harness_window, WM_KEYDOWN, VK_SHIFT, 0x00360001);
	ok = ok && receive_input(h, &shift[0]) && receive_input(h, &shift[1]) && capture(h, false, 306);
	PostMessageW(harness_window, WM_APP, VK_LSHIFT, 0);
	PostMessageW(harness_window, WM_APP, VK_RSHIFT, 0);
	PostMessageW(harness_window, WM_KEYUP, VK_SHIFT, 0xc0360001);
	keys = game_keys;
	PostMessageW(harness_window, WM_KEYDOWN, 'B', 0x00300001);
	expect("OverlayCapture 0 gives input back to the game", ok && wait_count(&game_keys, keys));
	wait_presents(3);
	expect("the window procedure is restored after the capture",
		GetWindowLongPtrW(harness_window, GWLP_WNDPROC) == (LONG_PTR)harness_proc);
	check_input_backlog(h);

	// The shared memory section
	wchar_t name[64];
	_snwprintf_s(name, _TRUNCATE, L"Local\\nahida-overlay-test-%lu", GetCurrentProcessId());
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
		OVERLAY_SECTION_HEADER + W * 4 * H, name);
	uint8_t *view = mapping ? (uint8_t*)MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0) : NULL;
	if (!view) {
		expect("a shared memory section for the overlay", false);
	} else {
		uint32_t header[3] = { W, H, W * 4 };
		memcpy(view + 8, header, sizeof(header));
		for (UINT i = 0; i < W * H; i++)
			memcpy(view + OVERLAY_SECTION_HEADER + i * 4, &WHITE, 4);
		*(volatile uint64_t*)view = 1;
		set.kind = OVERLAY_SECTION;
		set.handle = 0;
		char narrow[64];
		WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), NULL, NULL);
		set.section = narrow;
		ok = send(h, MessageType::OverlaySetTexture, 307, set)
			&& receive_as(h, MessageType::OverlayTextureSet, 307, &set_reply);
		wait_presents(3);
		expect("a shared memory section is drawn", ok && near_rgb(read_pixel(40, 80), 255, 255, 255));
	}

	set.kind = OVERLAY_KMT;
	set.handle = 0x1234;
	set.section.clear();
	Error error;
	ok = send(h, MessageType::OverlaySetTexture, 308, set) && receive_as(h, MessageType::Error, 308, &error);
	expect("a handle that does not open is refused", ok && error.code == "OVERLAY_OPEN_FAILED");
	wait_presents(3);
	expect("a refused OverlaySetTexture leaves no overlay", near_rgb(read_pixel(40, 80), CLEAR[0], CLEAR[1], CLEAR[2]));

	// A reconnect drops the overlay and the capture
	set.kind = OVERLAY_KMT;
	set.handle = (uint32_t)(uintptr_t)handle;
	frame.seq = 4;
	ok = send(h, MessageType::OverlaySetTexture, 309, set)
		&& receive_as(h, MessageType::OverlayTextureSet, 309, &set_reply)
		&& send(h, MessageType::OverlayFrame, 310, frame) && capture(h, true, 311);
	expect("the overlay is set again before the reconnect", ok);
	CloseHandle(h);
	DWORD open_error;
	for (int retry = 0; retry < 500; retry++) {
		h = open_pipe(&open_error);
		if (h != INVALID_HANDLE_VALUE)
			break;
		Sleep(1);
	}
	*connection = h;
	Hello hello;
	hello.client = "overlay harness";
	hello.min_version = hello.max_version = 1;
	HelloReply info;
	Received r;
	OverlaySurface surface;
	ok = h != INVALID_HANDLE_VALUE && send(h, MessageType::Hello, 1, hello)
		&& receive_as(h, MessageType::HelloReply, 1, &info) && receive(h, &r);
	expect("OverlaySurface follows HelloReply", ok && r.header.type == (uint16_t)MessageType::OverlaySurface
		&& r.header.request_id == 0 && DecodePayload(r.payload.data(), r.payload.size(), &surface)
		&& surface.hwnd == (uint32_t)(uintptr_t)harness_window && surface.width == WIDTH && surface.height == HEIGHT
		&& surface.dpi > 0);
	moves = game_mouse_moves;
	wait_presents(3);
	PostMessageW(harness_window, WM_MOUSEMOVE, 0, MAKELPARAM(2, 2));
	expect("a reconnect ends the capture", wait_count(&game_mouse_moves, moves));
	expect("a reconnect drops the overlay", near_rgb(read_pixel(40, 80), CLEAR[0], CLEAR[1], CLEAR[2]));
	expect("a reconnect restores the window procedure",
		GetWindowLongPtrW(harness_window, GWLP_WNDPROC) == (LONG_PTR)harness_proc);

	if (view)
		UnmapViewOfFile(view);
	if (mapping)
		CloseHandle(mapping);
	rtv->Release();
	mutex->Release();
	resource->Release();
	shared->Release();
	ctx->Release();
	dev->Release();
}

static int srgb_encode(double linear)
{
	double s = linear <= 0.0031308 ? linear * 12.92 : 1.055 * pow(linear, 1 / 2.4) - 0.055;
	return (int)(s * 255 + 0.5);
}

// With a _SRGB back buffer the render target encodes what is drawn, so the
// overlay's pixels, which are sRGB already, must be decoded to come out
// unchanged, and blended in linear light with the alpha taken off first.
static void check_overlay_srgb(HANDLE h)
{
	const UINT W = 128, H = 96;
	const uint32_t GRAY = 0xff404040, HALF_WHITE = 0x80808080;
	// The render loop's clear color, linear on a _SRGB render target
	const double CLEAR[3] = { 0.1, 0.2, 0.3 };
	wchar_t path[MAX_PATH];
	GetSystemDirectoryW(path, MAX_PATH);
	wcscat_s(path, L"\\d3d11.dll");
	PFN_D3D11_CREATE_DEVICE create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(LoadLibraryW(path),
		"D3D11CreateDevice");
	ID3D11Device *dev = NULL;
	if (!create_device || FAILED(create_device(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev,
			NULL, NULL))) {
		expect("a second WARP device for the desktop side", false);
		return;
	}
	std::vector<uint32_t> pixels(W * H);
	for (UINT i = 0; i < W * H; i++)
		pixels[i] = i % W < W / 2 ? GRAY : HALF_WHITE;
	D3D11_TEXTURE2D_DESC desc = { W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX };
	D3D11_SUBRESOURCE_DATA data = { pixels.data(), W * 4, 0 };
	ID3D11Texture2D *shared;
	IDXGIResource *resource;
	HANDLE handle;
	CHECK(dev->CreateTexture2D(&desc, &data, &shared));
	CHECK(shared->QueryInterface(__uuidof(IDXGIResource), (void**)&resource));
	CHECK(resource->GetSharedHandle(&handle));

	OverlaySetTexture set;
	set.kind = OVERLAY_KMT;
	set.handle = (uint32_t)(uintptr_t)handle;
	set.width = W;
	set.height = H;
	OverlayTextureSet set_reply;
	OverlayFrame frame;
	frame.seq = 1;
	bool ok = send(h, MessageType::OverlaySetTexture, 400, set)
		&& receive_as(h, MessageType::OverlayTextureSet, 400, &set_reply)
		&& send(h, MessageType::OverlayFrame, 401, frame);
	wait_presents(3);
	expect("on a _SRGB back buffer the overlay keeps its colors", ok && near_rgb(read_pixel(40, 80), 64, 64, 64));
	int blended[3];
	for (int i = 0; i < 3; i++)
		blended[i] = srgb_encode(128 / 255.0 + CLEAR[i] * 127 / 255.0);
	expect("on a _SRGB back buffer premultiplied pixels blend in linear light",
		near_rgb(read_pixel(100, 80), blended[0], blended[1], blended[2]));
	expect("the game's geometry shader is bound again after the overlay", present_gs && !present_gs_lost);

	resource->Release();
	shared->Release();
	dev->Release();
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
	expect("HelloReply: version 1, this process, D3D11, VARS and OVERLAY", info.version == 1
		&& info.pid == GetCurrentProcessId() && info.render_api == RENDER_D3D11
		&& info.capabilities == (CAPABILITY_VARS | CAPABILITY_OVERLAY));
	expect("HelloReply: the importer folder is the run folder", !_stricmp(info.importer_folder.c_str(), run_folder.c_str()));
	printf("  DLL version %s, exe %s\n", info.dll_version.c_str(), info.exe_path.c_str());
	if (srgb_back_buffer) {
		hold_present = 0;
		check_overlay_srgb(h);
		CloseHandle(h);
		client_done = true;
		return 0;
	}

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
	check_overlay(&h);

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
		printf("channel.exe <run folder> [idle <seconds> | srgb]\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	char full[MAX_PATH];
	GetFullPathNameA(argv[1], MAX_PATH, full, NULL);
	run_folder = full;
	while (!run_folder.empty() && run_folder.back() == '\\')
		run_folder.pop_back();
	DWORD idle_ms = argc > 3 && !strcmp(argv[2], "idle") ? (DWORD)(atof(argv[3]) * 1000) : 0;
	srgb_back_buffer = argc > 2 && !strcmp(argv[2], "srgb");
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
	wc.lpfnWndProc = harness_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"channel-harness";
	RegisterClassW(&wc);
	HWND window = CreateWindowW(wc.lpszClassName, L"channel-harness", WS_OVERLAPPEDWINDOW, 40, 40,
			WIDTH, HEIGHT, NULL, NULL, wc.hInstance, NULL);
	harness_window = window;
	show_harness_window(window);

	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferDesc.Width = WIDTH;
	scd.BufferDesc.Height = HEIGHT;
	scd.BufferDesc.Format = srgb_back_buffer ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	// One buffer that keeps what was presented, for read_pixel()
	scd.BufferCount = 1;
	scd.OutputWindow = window;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
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
	ID3D11Texture2D *staging;
	D3D11_TEXTURE2D_DESC staging_desc = { 1, 1, 1, 1, scd.BufferDesc.Format, { 1, 0 }, D3D11_USAGE_STAGING, 0,
		D3D11_CPU_ACCESS_READ, 0 };
	CHECK(dev->CreateTexture2D(&staging_desc, NULL, &staging));
	ID3D11Predicate *predicate = NULL;
	if (srgb_back_buffer) {
		// Matches the output of SpriteBatch's vertex shader
		static const char gs_hlsl[] =
			"struct V { float4 color : COLOR0; float2 tex : TEXCOORD0; float4 pos : SV_Position; };\n"
			"[maxvertexcount(3)] void main(triangle V v[3], inout TriangleStream<V> s) {}\n";
		ID3DBlob *blob;
		CHECK(D3DCompile(gs_hlsl, sizeof(gs_hlsl) - 1, "present_gs", NULL, NULL, "main", "gs_4_0", 0, 0, &blob, NULL));
		CHECK(dev->CreateGeometryShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &present_gs));
		blob->Release();
	}

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
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			// Key repeats only unless translate_presses: other posted keys
			// carry characters of their own
			if (msg.message == WM_KEYDOWN && (translate_presses || (msg.lParam & (1 << 30))))
				TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (hold_present && frames_presented) {
			Sleep(1);
			continue;
		}
		ctx->OMSetRenderTargets(1, &rtv, NULL);
		ctx->ClearRenderTargetView(rtv, color);
		if (InterlockedExchange(&hide_window_cursor, 0))
			SetCursor(NULL);
		if (present_predicated) {
			// Nothing drawn: the predicate is FALSE, and rendering is skipped
			if (!predicate) {
				D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
				CHECK(dev->CreatePredicate(&qd, &predicate));
			}
			ctx->Begin(predicate);
			ctx->End(predicate);
			while (ctx->GetData(predicate, NULL, 0, 0) == S_FALSE)
				Sleep(0);
			ctx->SetPredication(predicate, FALSE);
		}
		ctx->GSSetShader(present_gs, NULL, 0);
		swap->Present(0, 0);
		if (present_gs) {
			ID3D11GeometryShader *bound;
			ctx->GSGetShader(&bound, NULL, NULL);
			if (bound != present_gs)
				present_gs_lost = 1;
			if (bound)
				bound->Release();
		}
		ctx->SetPredication(NULL, FALSE);
		window_cursor = GetCursor();
		if (readback_pending) {
			D3D11_BOX box = { (UINT)readback_x, (UINT)readback_y, 0, (UINT)readback_x + 1, (UINT)readback_y + 1, 1 };
			D3D11_MAPPED_SUBRESOURCE mapped;
			CHECK(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back));
			ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, back, 0, &box);
			back->Release();
			CHECK(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
			readback_pixel = *(uint32_t*)mapped.pData;
			ctx->Unmap(staging, 0);
			InterlockedExchange(&readback_pending, 0);
		}
		if (!frames_presented)
			first_present_ms = GetTickCount() - launch_tick;
		InterlockedIncrement(&frames_presented);
		Sleep(5);
	}
	if (predicate)
		predicate->Release();
	staging->Release();
	rtv->Release();
	ctx->Release();
	swap->Release();
	dev->Release();
	DestroyWindow(window);

	printf("%ld failures\n", failures);
	return failures ? 2 : 0;
}
