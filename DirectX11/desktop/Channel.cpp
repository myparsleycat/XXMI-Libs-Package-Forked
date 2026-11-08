#include "Channel.h"

#include <windows.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

#include "Pick.h"

#include "../CommandList.h"
#include "../globals.h"
#include "../HackerDXGI.h"
#include "../input.h"
#include "../IniHandler.h"
#include "../log.h"
#include "../Overlay.h"
#include "version.h"

#pragma comment(lib, "advapi32.lib")

using namespace DesktopProtocol;

bool desktop_channel_enabled;

namespace {

const size_t MAX_INBOUND = 16;
const size_t MAX_REPLIES = 64;
const size_t MAX_EVENTS = 32;
const DWORD PIPE_BUFFER = 64 * 1024;
const DWORD WRITE_TIMEOUT_MS = 5000;

// A request the render thread carries out at the next Present
struct Inbound {
	uint32_t connection;
	MessageType type;
	uint32_t request_id;
	GetVars get_vars;
	SetVars set_vars;
	PickArm pick_arm;
};

// Shared by the render thread and the pipe thread. The render thread only
// ever tries to take the lock, and the pipe thread holds it for no more than
// moving queue entries.
SRWLOCK queue_lock = SRWLOCK_INIT;
std::deque<Inbound> inbound;
std::deque<std::vector<uint8_t>> replies;
std::deque<std::vector<uint8_t>> events;
std::vector<uint8_t> result;
bool has_result;
uint32_t queue_connection;
// Includes accepted requests still in inbound, render-thread work or pending.
size_t reserved_replies;

std::atomic<uint32_t> connection_id;
HANDLE wake_event;
HANDLE pipe = INVALID_HANDLE_VALUE;
HelloReply hello_info;

// Separate swap chains can present on different threads. The pipe thread
// never takes this lock.
std::mutex frame_lock;
bool started;
uint8_t started_api;
HackerSwapChain *driving_swap_chain;
HWND driving_window;
bool key_pick;
struct Outbound {
	DesktopSend kind;
	std::vector<uint8_t> frame;
	uint32_t connection;
};
std::vector<Outbound> pending;

std::wstring module_folder(HMODULE module)
{
	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
	if (!n || n >= MAX_PATH)
		return L"";
	wchar_t *slash = wcsrchr(path, L'\\');
	if (slash)
		*slash = 0;
	return path;
}

template <class M> void queue_message(DesktopSend kind, MessageType type, uint32_t request_id, M &message, uint32_t connection)
{
	std::vector<uint8_t> frame;
	if (EncodeFrame(type, request_id, message, &frame))
		DesktopChannelQueue(kind, std::move(frame), connection);
	else
		LogInfo("Desktop channel: message 0x%04x does not encode\n", (unsigned)type);
}

void queue_error(uint32_t request_id, const char *code, const std::string &detail, uint32_t connection)
{
	Error e;
	e.code = code;
	e.detail = detail;
	queue_message(DesktopSend::REPLY, MessageType::Error, request_id, e, connection);
}

// Moves what the render thread queued to the pipe thread, unless that thread
// holds the lock right now, in which case it is tried again next time
void flush_pending()
{
	if (pending.empty() || !TryAcquireSRWLockExclusive(&queue_lock))
		return;
	if (queue_connection && connection_id.load() == queue_connection) {
		for (auto &p : pending) {
			if (p.connection != queue_connection)
				continue;
			switch (p.kind) {
			case DesktopSend::REPLY:
				replies.push_back(std::move(p.frame));
				break;
			case DesktopSend::EVENT:
				if (events.size() >= MAX_EVENTS)
					events.pop_front();
				events.push_back(std::move(p.frame));
				break;
			case DesktopSend::RESULT:
				result = std::move(p.frame);
				has_result = true;
				break;
			}
		}
	}
	ReleaseSRWLockExclusive(&queue_lock);
	pending.clear();
	SetEvent(wake_event);
}

CommandListVariable* find_variable(const std::string &name)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, name.data(), (int)name.size(), NULL, 0);
	std::wstring wide(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, name.data(), (int)name.size(), &wide[0], n);
	std::transform(wide.begin(), wide.end(), wide.begin(), ::towlower);
	auto found = command_list_globals.find(wide);
	return found == command_list_globals.end() ? NULL : &found->second;
}

void get_vars(const Inbound &in)
{
	VarsReply reply;
	for (const std::string &name : in.get_vars.names) {
		Var v;
		v.name = name;
		if (CommandListVariable *var = find_variable(name)) {
			v.exists = true;
			v.value = var->fval;
		}
		reply.values.push_back(v);
	}
	queue_message(DesktopSend::REPLY, MessageType::VarsReply, in.request_id, reply, in.connection);
}

void set_vars(const Inbound &in)
{
	VarsApplied applied;
	applied.frame = G->frame_no;
	for (const Assign &a : in.set_vars.vars) {
		Var v;
		v.name = a.name;
		if (CommandListVariable *var = find_variable(a.name)) {
			// A locked variable is not assigned to from an ini either
			if (!(var->flags & VariableFlags::LOCKED) && var->fval != a.value) {
				LogVariableWrite(&var->fval);
				var->fval = a.value;
				if (var->flags & VariableFlags::PERSIST)
					G->user_config_dirty = true;
			}
			v.exists = true;
			v.value = var->fval;
		}
		applied.values.push_back(v);
	}
	queue_message(DesktopSend::REPLY, MessageType::VarsApplied, in.request_id, applied, in.connection);
}

void pick_arm(const Inbound &in)
{
	uint64_t id = DesktopPickArm(TRIGGER_DESKTOP, in.pick_arm.cursor == 1, in.pick_arm.x, in.pick_arm.y, in.connection);
	if (!id) {
		queue_error(in.request_id, "BUSY", "", in.connection);
		return;
	}
	PickArmed armed;
	armed.capture_id = id;
	queue_message(DesktopSend::REPLY, MessageType::PickArmed, in.request_id, armed, in.connection);
}

void inspect_key_down(HackerDevice*, void*)
{
	key_pick = true;
}

// ----------------------------------------------------------- the pipe thread

HANDLE read_event, write_event, connect_event;

bool write_frame(const std::vector<uint8_t> &frame)
{
	OVERLAPPED ov = {};
	DWORD written = 0;

	ov.hEvent = write_event;
	if (!WriteFile(pipe, frame.data(), (DWORD)frame.size(), NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
		return false;
	// A client that stops reading costs the connection, not this thread
	if (WaitForSingleObject(write_event, WRITE_TIMEOUT_MS) != WAIT_OBJECT_0) {
		CancelIoEx(pipe, &ov);
		GetOverlappedResult(pipe, &ov, &written, TRUE);
		return false;
	}
	return GetOverlappedResult(pipe, &ov, &written, FALSE) && written == frame.size();
}

template <class M> bool write_message(MessageType type, uint32_t request_id, M &message)
{
	std::vector<uint8_t> frame;
	return EncodeFrame(type, request_id, message, &frame) && write_frame(frame);
}

bool write_error(uint32_t request_id, const char *code, const std::string &detail)
{
	Error e;
	e.code = code;
	e.detail = detail;
	return write_message(MessageType::Error, request_id, e);
}

bool queue_inbound(Inbound &&in)
{
	bool queued = false;
	AcquireSRWLockExclusive(&queue_lock);
	if (inbound.size() < MAX_INBOUND && reserved_replies < MAX_REPLIES && in.connection == queue_connection) {
		inbound.push_back(std::move(in));
		reserved_replies++;
		queued = true;
	}
	ReleaseSRWLockExclusive(&queue_lock);
	return queued;
}

template <class M> bool decode(const FrameHeader &h, const uint8_t *payload, M *m)
{
	return DecodePayload(payload, h.payload_size, m);
}

// False closes the connection
bool handle_frame(uint32_t connection, const FrameHeader &h, const uint8_t *payload, bool *hello_done)
{
	MessageType type = (MessageType)h.type;

	if (!*hello_done) {
		Hello hello;
		if (type != MessageType::Hello)
			return write_error(h.request_id, "NOT_READY", "");
		if (!decode(h, payload, &hello)) {
			write_error(h.request_id, "MALFORMED", "Hello");
			return false;
		}
		uint16_t version = min(hello.max_version, VERSION);
		if (hello.min_version > VERSION || hello.max_version < VERSION) {
			write_error(h.request_id, "UNSUPPORTED_VERSION", std::to_string(VERSION) + "-" + std::to_string(VERSION));
			return false;
		}
		HelloReply reply = hello_info;
		reply.version = version;
		if (!write_message(MessageType::HelloReply, h.request_id, reply))
			return false;
		LogInfo("Desktop channel: %s connected\n", hello.client.c_str());
		*hello_done = true;
		return true;
	}

	Inbound in = {};
	in.connection = connection;
	in.type = type;
	in.request_id = h.request_id;

	switch (type) {
	case MessageType::Ping: {
		Ping ping;
		if (!decode(h, payload, &ping)) {
			write_error(h.request_id, "MALFORMED", "Ping");
			return false;
		}
		Pong pong;
		pong.nonce = ping.nonce;
		return write_message(MessageType::Pong, h.request_id, pong);
	}
	case MessageType::GetVars:
		if (!decode(h, payload, &in.get_vars)) {
			write_error(h.request_id, "MALFORMED", "GetVars");
			return false;
		}
		// VarsReply adds an exists byte and f32 to each requested name.
		if (HEADER_SIZE + h.payload_size + in.get_vars.names.size() * 5 > MAX_FRAME_SIZE)
			return write_error(h.request_id, "RESPONSE_TOO_LARGE", "request fewer variables");
		break;
	case MessageType::SetVars:
		if (!decode(h, payload, &in.set_vars)) {
			write_error(h.request_id, "MALFORMED", "SetVars");
			return false;
		}
		// VarsApplied adds a frame number and an exists byte per assignment.
		if (HEADER_SIZE + h.payload_size + 8 + in.set_vars.vars.size() > MAX_FRAME_SIZE)
			return write_error(h.request_id, "RESPONSE_TOO_LARGE", "request fewer variables");
		break;
	case MessageType::PickArm:
		if (!decode(h, payload, &in.pick_arm) || in.pick_arm.cursor > 1) {
			write_error(h.request_id, "MALFORMED", "PickArm");
			return false;
		}
		if (!(hello_info.capabilities & CAPABILITY_PICK_D3D11))
			return write_error(h.request_id, "UNSUPPORTED", "picking needs D3D11");
		break;
	default:
		// Pong, Error, and types of later revisions
		return true;
	}
	if (!queue_inbound(std::move(in)))
		return write_error(h.request_id, "BUSY", "");
	return true;
}

bool handle_input(uint32_t connection, std::vector<uint8_t> &input, bool *hello_done)
{
	size_t used = 0;
	bool keep = true;

	while (keep && input.size() - used >= HEADER_SIZE) {
		FrameHeader h;
		if (!ParseHeader(input.data() + used, &h)) {
			write_error(0, "MALFORMED", "frame header");
			return false;
		}
		if (input.size() - used - HEADER_SIZE < h.payload_size)
			break;
		keep = handle_frame(connection, h, input.data() + used + HEADER_SIZE, hello_done);
		used += HEADER_SIZE + h.payload_size;
	}
	input.erase(input.begin(), input.begin() + used);
	return keep;
}

// Sends what the render thread queued. Before the handshake there is nothing
// the client asked for, and nothing it would accept, so it is dropped.
bool send_queued(bool hello_done)
{
	std::deque<std::vector<uint8_t>> out;

	AcquireSRWLockExclusive(&queue_lock);
	reserved_replies -= replies.size();
	out.swap(replies);
	while (!events.empty()) {
		out.push_back(std::move(events.front()));
		events.pop_front();
	}
	if (has_result) {
		out.push_back(std::move(result));
		has_result = false;
	}
	ReleaseSRWLockExclusive(&queue_lock);

	if (!hello_done)
		return true;
	for (auto &frame : out) {
		if (!write_frame(frame))
			return false;
	}
	return true;
}

void clear_queues(uint32_t connection)
{
	AcquireSRWLockExclusive(&queue_lock);
	inbound.clear();
	replies.clear();
	reserved_replies = 0;
	events.clear();
	result.clear();
	has_result = false;
	queue_connection = connection;
	ReleaseSRWLockExclusive(&queue_lock);
}

void serve_connection(uint32_t connection)
{
	std::vector<uint8_t> input;
	static uint8_t chunk[PIPE_BUFFER];
	OVERLAPPED ov = {};
	bool hello_done = false, reading = false, refused = false;
	DWORD n;

	clear_queues(connection);
	connection_id = connection;
	ov.hEvent = read_event;

	for (;;) {
		if (!reading) {
			if (!ReadFile(pipe, chunk, sizeof(chunk), NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
				break;
			reading = true;
		}
		HANDLE handles[2] = { read_event, wake_event };
		DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
		if (w == WAIT_OBJECT_0) {
			reading = false;
			if (!GetOverlappedResult(pipe, &ov, &n, FALSE))
				break;
			input.insert(input.end(), chunk, chunk + n);
			if (!handle_input(connection, input, &hello_done)) {
				refused = true;
				break;
			}
		} else if (w == WAIT_OBJECT_0 + 1) {
			if (!send_queued(hello_done))
				break;
		} else {
			break;
		}
	}

	connection_id = 0;
	// DisconnectNamedPipe drops what the client has not read yet, so the
	// Error that closed the connection waits up to a second for the client
	// to read it and close its end
	DWORD until = GetTickCount() + 1000;
	while (refused) {
		if (!reading) {
			if (!ReadFile(pipe, chunk, sizeof(chunk), NULL, &ov) && GetLastError() != ERROR_IO_PENDING)
				break;
			reading = true;
		}
		DWORD left = until - GetTickCount();
		if ((int)left <= 0 || WaitForSingleObject(read_event, left) != WAIT_OBJECT_0)
			break;
		reading = false;
		if (!GetOverlappedResult(pipe, &ov, &n, FALSE))
			break;
	}
	if (reading) {
		CancelIoEx(pipe, &ov);
		GetOverlappedResult(pipe, &ov, &n, TRUE);
	}
	clear_queues(0);
	LogInfo("Desktop channel: client disconnected\n");
}

// Read and write for the user the game runs as, at medium integrity, so that
// a desktop that is not elevated reaches a game that is
bool make_security(SECURITY_ATTRIBUTES *sa)
{
	HANDLE token;
	BYTE buffer[256];
	DWORD size;
	wchar_t *sid = NULL;
	bool ok = false;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)
			&& ConvertSidToStringSidW(((TOKEN_USER*)buffer)->User.Sid, &sid)) {
		std::wstring sddl = L"D:P(A;;GRGW;;;" + std::wstring(sid) + L")S:(ML;;NW;;;ME)";
		ok = !!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
				&sa->lpSecurityDescriptor, NULL);
		LocalFree(sid);
	}
	CloseHandle(token);
	sa->nLength = sizeof(*sa);
	sa->bInheritHandle = FALSE;
	return ok;
}

DWORD WINAPI serve(LPVOID)
{
	SECURITY_ATTRIBUTES sa = {};
	wchar_t name[64];
	uint32_t connection = 0;

	if (!make_security(&sa)) {
		LogInfo("Desktop channel: no security descriptor for the pipe: %lu\n", GetLastError());
		return 0;
	}
	_snwprintf_s(name, _TRUNCATE, L"\\\\.\\pipe\\nahida-migoto-%lu", GetCurrentProcessId());
	// FILE_FLAG_FIRST_PIPE_INSTANCE: a pipe of this name some other process
	// made first is not ours to serve
	pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, PIPE_BUFFER, PIPE_BUFFER, 0, &sa);
	LocalFree(sa.lpSecurityDescriptor);
	if (pipe == INVALID_HANDLE_VALUE) {
		LogInfo("Desktop channel: cannot create %S: %lu\n", name, GetLastError());
		return 0;
	}
	LogInfo("Desktop channel: listening on %S\n", name);

	for (;;) {
		OVERLAPPED ov = {};
		DWORD n;
		ov.hEvent = connect_event;
		BOOL ok = ConnectNamedPipe(pipe, &ov);
		DWORD err = ok ? ERROR_SUCCESS : GetLastError();
		if (err == ERROR_IO_PENDING) {
			WaitForSingleObject(connect_event, INFINITE);
			ok = GetOverlappedResult(pipe, &ov, &n, FALSE);
			err = ok ? ERROR_SUCCESS : GetLastError();
		}
		if (err == ERROR_SUCCESS || err == ERROR_PIPE_CONNECTED)
			serve_connection(++connection);
		else
			Sleep(100);
		DisconnectNamedPipe(pipe);
	}
}

void start(uint8_t render_api)
{
	HMODULE self;

	started = true;
	started_api = render_api;

	hello_info.dll_version = VERSION_TAG_STR;
	hello_info.pid = GetCurrentProcessId();
	std::wstring exe(MAX_PATH, L'\0');
	exe.resize(GetModuleFileNameW(NULL, &exe[0], MAX_PATH));
	hello_info.exe_path = DesktopUtf8(exe.c_str());
	hello_info.importer_folder = DesktopUtf8(module_folder(migoto_handle).c_str());
	hello_info.render_api = render_api;
	hello_info.capabilities = CAPABILITY_VARS | (render_api == RENDER_D3D11 ? CAPABILITY_PICK_D3D11 : 0);

	wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	connect_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!wake_event || !read_event || !write_event || !connect_event) {
		LogInfo("Desktop channel: CreateEvent failed: %lu\n", GetLastError());
		return;
	}
	// The thread runs until the process ends and is never waited for, so
	// the DLL must not be unloaded under it
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&serve, &self);
	HANDLE thread = CreateThread(NULL, 0, serve, NULL, 0, NULL);
	if (thread)
		CloseHandle(thread);
	else
		LogInfo("Desktop channel: CreateThread failed: %lu\n", GetLastError());
}

} // namespace

std::string DesktopUtf8(const wchar_t *text, size_t length)
{
	if (length == (size_t)-1)
		length = wcslen(text);
	// Well inside the wire limit for a string, cut at a whole character
	if (length > 16384) {
		length = 16384;
		if (IS_HIGH_SURROGATE(text[length - 1]))
			length--;
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL);
	std::string out(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, (int)length, &out[0], n, NULL, NULL);
	return out;
}

void ParseDesktopSection()
{
	static bool loaded;
	bool channel;

	LogInfo("[Desktop]\n");
	channel = GetIniBool(L"Desktop", L"channel", false, NULL);
	// Shaders are noted for picking as they are created, so the channel
	// cannot start partway through a game
	if (!loaded)
		desktop_channel_enabled = channel;
	else if (channel != desktop_channel_enabled)
		LogOverlay(LOG_INFO, "[Desktop] channel takes effect at the next game start\n");
	loaded = true;

	if (desktop_channel_enabled)
		RegisterIniKeyBinding(L"Desktop", L"inspect_key", inspect_key_down, NULL, 0, NULL);
}

void DesktopChannelFrame(HackerDevice *device, HackerContext *hacker_context, uint8_t render_api,
	HackerSwapChain *swap_chain)
{
	std::lock_guard<std::mutex> lock(frame_lock);
	std::deque<Inbound> work;

	if (started && render_api != started_api)
		return;
	if (render_api == RENDER_D3D11) {
		if (!swap_chain || (driving_swap_chain && driving_swap_chain != swap_chain))
			return;
		if (!driving_swap_chain) {
			DXGI_SWAP_CHAIN_DESC desc;
			if (FAILED(swap_chain->GetDesc(&desc)))
				return;
			driving_swap_chain = swap_chain;
			driving_window = desc.OutputWindow;
		}
	}
	if (!started)
		start(render_api);

	if (TryAcquireSRWLockExclusive(&queue_lock)) {
		work.swap(inbound);
		ReleaseSRWLockExclusive(&queue_lock);
	}
	for (const Inbound &in : work) {
		if (in.connection != connection_id.load())
			continue;
		switch (in.type) {
		case MessageType::GetVars:
			get_vars(in);
			break;
		case MessageType::SetVars:
			set_vars(in);
			break;
		case MessageType::PickArm:
			pick_arm(in);
			break;
		}
	}

	if (key_pick) {
		key_pick = false;
		if (!DesktopChannelConnected())
			LogOverlay(LOG_INFO, "Hash inspector: Nahida Desktop is not connected to this game\n");
		else
			DesktopPickArm(TRIGGER_KEY, false, 0, 0, DesktopChannelConnection());
	}

	if (render_api == RENDER_D3D11)
		DesktopPickFrame(device, hacker_context, driving_window);
	else
		DesktopPickFrame(NULL, NULL, NULL);

	flush_pending();
}

void DesktopChannelSwapChainReleased(HackerSwapChain *swap_chain)
{
	std::lock_guard<std::mutex> lock(frame_lock);
	if (swap_chain != driving_swap_chain)
		return;
	driving_swap_chain = NULL;
	driving_window = NULL;
	DesktopPickCancel();
}

void DesktopChannelReloaded()
{
	std::lock_guard<std::mutex> lock(frame_lock);
	if (!desktop_channel_enabled || !started)
		return;
	Event e;
	e.kind = EVENT_RELOADED;
	e.frame = G->frame_no;
	queue_message(DesktopSend::EVENT, MessageType::Event, 0, e, DesktopChannelConnection());
	flush_pending();
}

void DesktopChannelDetach(bool process_terminating)
{
	// Only when the process ends are the other threads gone, so that this
	// write cannot land inside one of the pipe thread's
	if (!process_terminating || !DesktopChannelConnected() || pipe == INVALID_HANDLE_VALUE)
		return;
	static OVERLAPPED ov;
	static std::vector<uint8_t> frame;
	Event e;
	e.kind = EVENT_SHUTDOWN;
	e.frame = G->frame_no;
	if (EncodeFrame(MessageType::Event, 0, e, &frame))
		WriteFile(pipe, frame.data(), (DWORD)frame.size(), NULL, &ov);
}

bool DesktopChannelConnected()
{
	return DesktopChannelConnection() != 0;
}

uint32_t DesktopChannelConnection()
{
	return connection_id.load();
}

void DesktopChannelQueue(DesktopSend kind, std::vector<uint8_t> &&frame, uint32_t connection)
{
	pending.push_back({ kind, std::move(frame), connection });
	flush_pending();
}
