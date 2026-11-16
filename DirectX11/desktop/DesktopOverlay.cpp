#include "DesktopOverlay.h"

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include "Channel.h"

#include "../cursor.h"
#include "../HackerContext.h"
#include "../HackerDevice.h"
#include "../HackerDXGI.h"
#include "../log.h"
#include "../Overlay.h"

using namespace DesktopProtocol;

namespace {

// ShowCursor(TRUE) calls a capture makes at most against a game that hid the
// cursor with ShowCursor(FALSE)
const int MAX_SHOW_CURSOR = 64;
// The window procedure the subclass forwards to, set on every install and
// never removed, so that a message already inside overlay_proc while the
// subclass is removed still reaches the game
const wchar_t PROP_ORIGINAL[] = L"NahidaOverlayOriginalProc";
// Present while overlay_proc is in the window's chain of procedures
const wchar_t PROP_HOOKED[] = L"NahidaOverlayHooked";

struct Texture {
	uint8_t kind;
	uint32_t width, height;
	ID3D11Texture2D *shared;
	IDXGIKeyedMutex *mutex;
	HANDLE mapping;
	const uint8_t *view;
	SIZE_T view_size;
	ID3D11Texture2D *copy;
	// OVERLAY_SECTION: the pixels are read into this first, so that a frame
	// torn by a write during the read never reaches copy
	ID3D11Texture2D *upload;
	ID3D11ShaderResourceView *srv;
	bool has_content;
	// OVERLAY_KMT: a frame waits to be copied, and the OverlayFrame seq
	// seen last
	bool dirty;
	uint64_t frame_seq;
	// OVERLAY_SECTION: the header seq copied last
	uint64_t section_seq;
};

// Render thread
Texture texture;
ID3D11Device1 *device;
ID3D11DeviceContext1 *context;
uint32_t overlay_connection;
HWND window;
ID3D11Device *luid_device;
LUID luid;
// The game's cursor clip when the capture started
RECT saved_clip;
bool clip_saved;

// Pipe thread to render thread
std::atomic<uint64_t> frame_seq;

// Render thread to pipe thread
SRWLOCK surface_lock = SRWLOCK_INIT;
OverlaySurface surface;
bool surface_valid;

// Render thread and the window's thread
UINT cursor_message;
// Posted while the window procedure handles a press of a key the desktop
// received, behind the characters TranslateMessage queued for that press
UINT chars_done_message;
std::atomic<HWND> hooked_window;
std::atomic<uint32_t> capture_connection;
// Held by the window's thread from ending the capture until its capture ended
// is queued, and by the render thread while it starts one. The connection id
// does not tell captures apart, so without it the end of the previous capture
// would close the input of the next.
SRWLOCK capture_lock = SRWLOCK_INIT;
// The connection whose capture lost a release the client was behind on. The
// next Present ends that capture.
std::atomic<uint32_t> input_lost;
std::atomic<uint8_t> capture_cursor;
std::atomic<bool> cursor_posted;
// ShowCursor(TRUE) calls the window's thread made for the capture
std::atomic<int> shown_count;
// The window's thread holds the mouse capture for a pressed button
std::atomic<bool> mouse_captured;
// The window's thread set the overlay's cursor, which a game that hides it
// with a transparent cursor keeps until CursorRestoreGame
std::atomic<bool> cursor_set;
// Counts the times overlay_proc started to see the window's messages, so that
// the window's thread forgets what it recorded while it saw none
std::atomic<uint32_t> hook_generation;
// A key or button whose press went to the desktop is held or a character of
// such a key may still be queued, and the subclass stays to keep them from
// the game
std::atomic<bool> desktop_held;
// The cursor was last over the window's client area during the capture
std::atomic<bool> cursor_in_client;
// Window's thread
WCHAR high_surrogate;
// Keys and buttons whose press went to the desktop, in hook held_generation,
// keys by key_index. The release of anything else belongs to a press the game
// received.
bool desktop_keys[768];
bool desktop_buttons[BUTTON_FORWARD + 1];
// Scan codes (bit 8 extended) of keys held since before the capture whose
// repeats arrived. TranslateMessage queues their characters before the
// repeat reaches the window procedure.
bool game_repeat_scans[512];
// chars_done_message posts not yet received, per scan code. A character that
// arrives before them belongs to a press the desktop received and is kept
// from the game outside a capture.
uint16_t desktop_char_markers[512];
uint32_t held_generation;
// The cursor the capture replaced and the one it set, for a game whose cursor
// no SetCursor hook records
HCURSOR game_cursor;
HCURSOR overlay_cursor;
bool game_cursor_saved;
// The game's raw input registrations of the mouse and keyboard with
// RIDEV_NOLEGACY, which the capture registers without it, since it reads the
// legacy messages that flag suppresses. raw_checked: the registrations were
// looked at during this capture. Kept across a change of window: the thread
// of the next window puts back what a destroyed window left lifted.
RAWINPUTDEVICE raw_saved[2];
UINT raw_saved_count;
bool raw_checked;
// Render thread and the window's thread: registrations wait to be put back,
// and the subclass stays until they are
std::atomic<bool> raw_lifted;
// Window's thread, by virtual-key code with left and right modifiers apart.
// Raw keyboard input has no repeat flag, and the physical key state runs ahead
// of the queue, so a press before the first release seen in the capture may be
// a repeat of a press the game received: its release goes to the game, which
// at worst sees a release of a key it never saw pressed.
enum RawKey : uint8_t { RAW_KEY_UNSEEN, RAW_KEY_UP, RAW_KEY_DESKTOP };
RawKey raw_keys[256];
// Buttons do not repeat: a raw press during the capture is the desktop's
bool raw_desktop_buttons[5];

uint32_t current_mods()
{
	uint32_t mods = 0;
	if (GetKeyState(VK_SHIFT) < 0)
		mods |= MOD_SHIFT_BIT;
	if (GetKeyState(VK_CONTROL) < 0)
		mods |= MOD_CTRL_BIT;
	if (GetKeyState(VK_MENU) < 0)
		mods |= MOD_ALT_BIT;
	if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
		mods |= MOD_WIN_BIT;
	return mods;
}

// ------------------------------------------------------------------ texture

void release_texture()
{
	if (texture.srv)
		texture.srv->Release();
	if (texture.copy)
		texture.copy->Release();
	if (texture.upload)
		texture.upload->Release();
	if (texture.mutex)
		texture.mutex->Release();
	if (texture.shared)
		texture.shared->Release();
	if (texture.view)
		UnmapViewOfFile(texture.view);
	if (texture.mapping)
		CloseHandle(texture.mapping);
	texture = {};
}

std::string hresult(const char *call, HRESULT hr)
{
	char buf[64];
	_snprintf_s(buf, _TRUNCATE, "%s 0x%08x", call, (unsigned)hr);
	return buf;
}

std::string win32_error(const char *call)
{
	char buf[64];
	_snprintf_s(buf, _TRUNCATE, "%s %lu", call, GetLastError());
	return buf;
}

bool open_kmt(const OverlaySetTexture &request, const char **code, std::string *detail)
{
	HANDLE handle = (HANDLE)(uintptr_t)request.handle;
	HRESULT hr = device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void**)&texture.shared);
	if (FAILED(hr)) {
		*code = "OVERLAY_OPEN_FAILED";
		*detail = hresult("OpenSharedResource", hr);
		return false;
	}

	D3D11_TEXTURE2D_DESC desc;
	texture.shared->GetDesc(&desc);
	if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.Width != request.width || desc.Height != request.height
			|| desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1
			|| !(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) {
		char buf[160];
		_snprintf_s(buf, _TRUNCATE, "texture is %ux%u format %u mips %u array %u samples %u misc 0x%x",
			desc.Width, desc.Height, desc.Format, desc.MipLevels, desc.ArraySize, desc.SampleDesc.Count,
			desc.MiscFlags);
		*code = "OVERLAY_OPEN_FAILED";
		*detail = buf;
		return false;
	}
	hr = texture.shared->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&texture.mutex);
	if (FAILED(hr)) {
		*code = "OVERLAY_OPEN_FAILED";
		*detail = hresult("IDXGIKeyedMutex", hr);
		return false;
	}
	// The desktop may have drawn before it sent the texture
	texture.dirty = true;
	texture.frame_seq = frame_seq.load();
	return true;
}

bool open_section(const OverlaySetTexture &request, const char **code, std::string *detail)
{
	*code = "OVERLAY_OPEN_FAILED";
	int n = MultiByteToWideChar(CP_UTF8, 0, request.section.data(), (int)request.section.size(), NULL, 0);
	std::wstring name(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, request.section.data(), (int)request.section.size(), &name[0], n);
	if (name.empty()) {
		*detail = "no section name";
		return false;
	}

	texture.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
	if (!texture.mapping) {
		*detail = win32_error("OpenFileMapping");
		return false;
	}
	texture.view = (const uint8_t*)MapViewOfFile(texture.mapping, FILE_MAP_READ, 0, 0, 0);
	if (!texture.view) {
		*detail = win32_error("MapViewOfFile");
		return false;
	}
	MEMORY_BASIC_INFORMATION info;
	if (!VirtualQuery(texture.view, &info, sizeof(info))) {
		*detail = win32_error("VirtualQuery");
		return false;
	}
	texture.view_size = info.RegionSize;
	uint64_t needed = OVERLAY_SECTION_HEADER + (uint64_t)request.width * 4 * request.height;
	if (texture.view_size < needed) {
		char buf[96];
		_snprintf_s(buf, _TRUNCATE, "section is %zu bytes, %llu needed", texture.view_size, needed);
		*detail = buf;
		return false;
	}
	return true;
}

bool create_copy(const char **code, std::string *detail)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = texture.width;
	desc.Height = texture.height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	*code = "OVERLAY_OPEN_FAILED";
	HRESULT hr = device->CreateTexture2D(&desc, NULL, &texture.copy);
	if (FAILED(hr)) {
		*detail = hresult("CreateTexture2D", hr);
		return false;
	}
	if (texture.kind == OVERLAY_SECTION) {
		desc.BindFlags = 0;
		hr = device->CreateTexture2D(&desc, NULL, &texture.upload);
		if (FAILED(hr)) {
			*detail = hresult("CreateTexture2D", hr);
			return false;
		}
	}
	hr = device->CreateShaderResourceView(texture.copy, NULL, &texture.srv);
	if (FAILED(hr)) {
		*detail = hresult("CreateShaderResourceView", hr);
		return false;
	}
	return true;
}

// The desktop sets a new texture after an OverlaySurface
void request_texture()
{
	AcquireSRWLockShared(&surface_lock);
	bool valid = surface_valid;
	OverlaySurface s = surface;
	ReleaseSRWLockShared(&surface_lock);
	if (valid && overlay_connection)
		DesktopChannelQueueMessage(DesktopSend::EVENT, MessageType::OverlaySurface, 0, s, overlay_connection);
}

uint64_t section_seq()
{
	return *(const volatile uint64_t*)texture.view;
}

// A copy skipped under the game's predicate would count as done
struct NoPredication {
	ID3D11Predicate *predicate = NULL;
	BOOL value = FALSE;

	NoPredication()
	{
		context->GetPredication(&predicate, &value);
		if (predicate)
			context->SetPredication(NULL, FALSE);
	}
	~NoPredication()
	{
		if (predicate) {
			context->SetPredication(predicate, value);
			predicate->Release();
		}
	}
};

void update_copy()
{
	if (texture.kind == OVERLAY_KMT) {
		uint64_t seq = frame_seq.load();
		if (seq != texture.frame_seq) {
			texture.frame_seq = seq;
			texture.dirty = true;
		}
		if (!texture.dirty)
			return;
		// The desktop holds key 0 while it draws. Never wait for it:
		// the previous copy is drawn and the next Present tries again.
		HRESULT hr = texture.mutex->AcquireSync(0, 0);
		if (hr == WAIT_ABANDONED) {
			// The holder of the key exited without releasing it: the
			// texture and its mutex are inconsistent and must be opened anew
			LogInfo("Desktop overlay: keyed mutex abandoned, texture dropped\n");
			release_texture();
			request_texture();
			return;
		}
		if (hr != S_OK)
			return;
		NoPredication no_predication;
		context->CopyResource(texture.copy, texture.shared);
		texture.mutex->ReleaseSync(0);
		texture.dirty = false;
		texture.has_content = true;
	} else if (texture.kind == OVERLAY_SECTION) {
		uint64_t seq = section_seq();
		// 0 while the desktop writes the pixels
		if (seq == 0 || seq == texture.section_seq)
			return;
		MemoryBarrier();
		uint32_t header[3];
		memcpy(header, texture.view + 8, sizeof(header));
		uint32_t width = header[0], height = header[1], stride = header[2];
		if (width != texture.width || height != texture.height || stride < width * 4
				|| OVERLAY_SECTION_HEADER + (uint64_t)stride * (height - 1) + width * 4 > texture.view_size)
			return;
		NoPredication no_predication;
		context->UpdateSubresource(texture.upload, 0, NULL, texture.view + OVERLAY_SECTION_HEADER, stride, 0);
		MemoryBarrier();
		// Written while we read: the next Present reads it again
		if (section_seq() != seq)
			return;
		context->CopyResource(texture.copy, texture.upload);
		texture.section_seq = seq;
		texture.has_content = true;
	}
}

// ------------------------------------------------------------------ surface

UINT window_dpi(HWND hwnd)
{
	typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
	static GetDpiForWindowFn get_dpi = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
		"GetDpiForWindow");
	UINT dpi = get_dpi ? get_dpi(hwnd) : 0;
	return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

LUID adapter_luid()
{
	if (device == luid_device)
		return luid;
	luid = {};
	luid_device = device;
	IDXGIDevice *dxgi_device;
	if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device)))
		return luid;
	IDXGIAdapter *adapter;
	if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
		DXGI_ADAPTER_DESC desc;
		if (SUCCEEDED(adapter->GetDesc(&desc)))
			luid = desc.AdapterLuid;
		adapter->Release();
	}
	dxgi_device->Release();
	return luid;
}

void update_surface(HackerSwapChain *swap_chain, uint32_t connection)
{
	DXGI_SWAP_CHAIN_DESC desc;
	if (FAILED(swap_chain->GetOrigSwapChain1()->GetDesc(&desc)))
		return;
	window = desc.OutputWindow;

	OverlaySurface s;
	LUID l = adapter_luid();
	s.hwnd = (uint32_t)(uintptr_t)desc.OutputWindow;
	s.width = desc.BufferDesc.Width;
	s.height = desc.BufferDesc.Height;
	s.dpi = window_dpi(desc.OutputWindow);
	s.luid_low = l.LowPart;
	s.luid_high = l.HighPart;

	AcquireSRWLockExclusive(&surface_lock);
	bool changed = !surface_valid || memcmp(&s, &surface, sizeof(s)) != 0;
	surface = s;
	surface_valid = true;
	ReleaseSRWLockExclusive(&surface_lock);
	if (changed && connection)
		DesktopChannelQueueMessage(DesktopSend::EVENT, MessageType::OverlaySurface, 0, s, connection);
}

// -------------------------------------------------------------- input capture

HCURSOR cursor_handle()
{
	LPCWSTR id;
	switch (capture_cursor.load()) {
	case CURSOR_IBEAM:       id = IDC_IBEAM; break;
	case CURSOR_HAND:        id = IDC_HAND; break;
	case CURSOR_SIZE_WE:     id = IDC_SIZEWE; break;
	case CURSOR_SIZE_NS:     id = IDC_SIZENS; break;
	case CURSOR_NOT_ALLOWED: id = IDC_NO; break;
	case CURSOR_WAIT:        id = IDC_WAIT; break;
	default:                 id = IDC_ARROW; break;
	}
	return LoadCursorW(NULL, id);
}

// Window's thread
void set_cursor()
{
	cursor_set = true;
	HCURSOR cursor = cursor_handle();
	HCURSOR previous = CursorBypass_SetCursor(cursor);
	// The game may set its own cursor during the capture
	if (!game_cursor_saved || previous != overlay_cursor) {
		game_cursor = previous;
		game_cursor_saved = true;
	}
	overlay_cursor = cursor;
}

// Window's thread. ShowCursor counts belong to the thread that calls it, which
// for a game that hides the cursor is the thread of its window.
void show_cursor()
{
	// Before ShowCursor, so that try_unhook waits for unshow_cursor
	cursor_set = true;
	CURSORINFO info = { sizeof(info) };
	if (GetCursorInfo(&info) && !(info.flags & CURSOR_SHOWING)) {
		while (shown_count.load() < MAX_SHOW_CURSOR) {
			shown_count++;
			if (ShowCursor(TRUE) >= 0)
				break;
		}
	}
	set_cursor();
}

void unshow_cursor()
{
	while (shown_count.load() > 0) {
		ShowCursor(FALSE);
		shown_count--;
	}
	if (!CursorRestoreGame() && game_cursor_saved && GetCursor() == overlay_cursor)
		SetCursor(game_cursor);
	game_cursor_saved = false;
	cursor_set = false;
}

// Window's thread
void release_mouse()
{
	if (mouse_captured.exchange(false))
		ReleaseCapture();
}

// The physical key: the left and right Shift, Ctrl and Alt share a virtual-key
// code, and an IME reports a press as VK_PROCESSKEY. Input without a scan code
// falls back to the virtual-key code.
unsigned key_index(WPARAM wp, LPARAM lp)
{
	unsigned scan = (lp >> 16) & 0x1ff;
	return scan ? scan : 512 + (unsigned)(wp & 0xff);
}

bool desktop_button_held()
{
	return std::find(std::begin(desktop_buttons), std::end(desktop_buttons), true) != std::end(desktop_buttons);
}

void note_desktop_held()
{
	desktop_held = desktop_button_held()
		|| std::find(std::begin(desktop_keys), std::end(desktop_keys), true) != std::end(desktop_keys)
		|| std::find_if(std::begin(desktop_char_markers), std::end(desktop_char_markers),
			[](uint16_t n) { return n != 0; }) != std::end(desktop_char_markers);
}

// For a press the desktop received. The window procedure runs after
// TranslateMessage queued the press's characters, so the message lands
// behind them.
void post_chars_done(HWND hwnd, unsigned scan)
{
	if (desktop_char_markers[scan] < UINT16_MAX && PostMessageW(hwnd, chars_done_message, scan, held_generation))
		desktop_char_markers[scan]++;
}

void chars_done(WPARAM scan, LPARAM generation)
{
	if ((uint32_t)generation != held_generation || scan >= ARRAYSIZE(desktop_char_markers)
			|| !desktop_char_markers[scan])
		return;
	desktop_char_markers[scan]--;
	note_desktop_held();
}

// The releases go to whichever window has the focus next
void forget_desktop_input()
{
	memset(desktop_keys, 0, sizeof(desktop_keys));
	memset(desktop_buttons, 0, sizeof(desktop_buttons));
	note_desktop_held();
}

// HID generic desktop page: mouse and keyboard
bool legacy_raw_device(const RAWINPUTDEVICE &device)
{
	return device.usUsagePage == 0x01 && (device.usUsage == 0x02 || device.usUsage == 0x06)
		// RIDEV_NOLEGACY shares its bits with RIDEV_EXCLUDE and RIDEV_PAGEONLY
		&& (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY;
}

// The flags without RIDEV_NOLEGACY and those only valid with it.
// RIDEV_CAPTUREMOUSE has the value of the keyboard's RIDEV_NOHOTKEYS.
DWORD without_nolegacy(const RAWINPUTDEVICE &device)
{
	return device.dwFlags & ~(RIDEV_NOLEGACY | (device.usUsage == 0x02 ? RIDEV_CAPTUREMOUSE : RIDEV_APPKEYS));
}

std::vector<RAWINPUTDEVICE> registered_raw_devices()
{
	std::vector<RAWINPUTDEVICE> devices;
	UINT count = 0;
	if (GetRegisteredRawInputDevices(NULL, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1 || !count)
		return devices;
	devices.resize(count);
	UINT got = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
	devices.resize(got == (UINT)-1 ? 0 : got);
	return devices;
}

// Window's thread, at the start of a capture
void lift_nolegacy()
{
	raw_checked = true;
	RAWINPUTDEVICE lifted[ARRAYSIZE(raw_saved)];
	UINT count = 0;
	for (const RAWINPUTDEVICE &device : registered_raw_devices()) {
		if (!legacy_raw_device(device) || count == ARRAYSIZE(raw_saved))
			continue;
		raw_saved[count] = device;
		lifted[count] = device;
		lifted[count].dwFlags = without_nolegacy(device);
		count++;
	}
	if (!count)
		return;
	if (!RegisterRawInputDevices(lifted, count, sizeof(RAWINPUTDEVICE))) {
		LogInfo("Desktop overlay: cannot register raw input without RIDEV_NOLEGACY: %lu\n", GetLastError());
		return;
	}
	raw_saved_count = count;
	raw_lifted = true;
}

// Window's thread. Puts back the registrations that are still as the capture
// registered them, also those of a window destroyed during the capture: one
// with no target window follows the keyboard focus and outlives it. Tells
// which devices were put back.
void restore_raw_registrations(bool *keyboard, bool *mouse)
{
	*keyboard = *mouse = false;
	if (!raw_lifted.load())
		return;
	std::vector<RAWINPUTDEVICE> current = registered_raw_devices();
	for (UINT i = 0; i < raw_saved_count; i++) {
		const RAWINPUTDEVICE &saved = raw_saved[i];
		// A registration the game made during the capture stays, and one whose
		// window is gone receives nothing
		auto it = std::find_if(current.begin(), current.end(), [&](const RAWINPUTDEVICE &device) {
			return device.usUsagePage == saved.usUsagePage && device.usUsage == saved.usUsage;
		});
		if (it == current.end() || it->dwFlags != without_nolegacy(saved) || it->hwndTarget != saved.hwndTarget
				|| (saved.hwndTarget && !IsWindow(saved.hwndTarget)))
			continue;
		if (!RegisterRawInputDevices(&saved, 1, sizeof(RAWINPUTDEVICE))) {
			LogInfo("Desktop overlay: cannot restore the game's raw input registration of usage %u: %lu\n",
				saved.usUsage, GetLastError());
			continue;
		}
		(saved.usUsage == 0x06 ? *keyboard : *mouse) = true;
	}
	raw_saved_count = 0;
	raw_lifted = false;
}

// Window's thread, once the capture ended
void restore_nolegacy()
{
	raw_checked = false;
	bool keyboard, mouse;
	restore_raw_registrations(&keyboard, &mouse);
	// No legacy message brings the releases of what the desktop received
	// anymore
	if (keyboard)
		memset(desktop_keys, 0, sizeof(desktop_keys));
	if (mouse) {
		memset(desktop_buttons, 0, sizeof(desktop_buttons));
		release_mouse();
	}
	note_desktop_held();
}

// Window's thread, at the start of a capture
void start_raw_tracking()
{
	memset(raw_keys, RAW_KEY_UNSEEN, sizeof(raw_keys));
	memset(raw_desktop_buttons, 0, sizeof(raw_desktop_buttons));
}

unsigned raw_key_vk(const RAWKEYBOARD &key)
{
	bool e0 = (key.Flags & RI_KEY_E0) != 0;
	switch (key.VKey) {
	case VK_SHIFT: return key.MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
	case VK_CONTROL: return e0 ? VK_RCONTROL : VK_LCONTROL;
	case VK_MENU: return e0 ? VK_RMENU : VK_LMENU;
	}
	return key.VKey;
}

// Window's thread, during a capture: true when the raw input message carries
// the release of a key or button the game received pressed. Such a mouse
// message goes to the game whole, with its movement and other button changes.
bool raw_release_for_game(LPARAM lp)
{
	RAWINPUT raw;
	UINT size = sizeof(raw);
	if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
		return false;

	if (raw.header.dwType == RIM_TYPEKEYBOARD) {
		unsigned vk = raw_key_vk(raw.data.keyboard);
		// 0xff: the fake keys of an escape sequence
		if (!vk || vk >= 0xff)
			return false;
		if (!(raw.data.keyboard.Flags & RI_KEY_BREAK)) {
			if (raw_keys[vk] == RAW_KEY_UP)
				raw_keys[vk] = RAW_KEY_DESKTOP;
			return false;
		}
		bool desktop = raw_keys[vk] == RAW_KEY_DESKTOP;
		raw_keys[vk] = RAW_KEY_UP;
		return !desktop;
	}

	if (raw.header.dwType != RIM_TYPEMOUSE)
		return false;
	USHORT flags = raw.data.mouse.usButtonFlags;
	bool game = false;
	for (int b = 0; b < (int)ARRAYSIZE(raw_desktop_buttons); b++) {
		if ((flags & (RI_MOUSE_BUTTON_1_UP << (2 * b))) && !raw_desktop_buttons[b])
			game = true;
	}
	for (int b = 0; b < (int)ARRAYSIZE(raw_desktop_buttons); b++) {
		if (flags & (RI_MOUSE_BUTTON_1_UP << (2 * b)))
			raw_desktop_buttons[b] = false;
		else if (flags & (RI_MOUSE_BUTTON_1_DOWN << (2 * b)))
			raw_desktop_buttons[b] = !game;
	}
	return game;
}

// Window's thread
void sync_nolegacy(bool capturing)
{
	if (capturing && !raw_checked) {
		start_raw_tracking();
		lift_nolegacy();
	}
	else if (!capturing && raw_checked)
		restore_nolegacy();
}

void sync_hook_generation()
{
	uint32_t generation = hook_generation.load();
	if (held_generation == generation)
		return;
	held_generation = generation;
	memset(desktop_keys, 0, sizeof(desktop_keys));
	memset(desktop_buttons, 0, sizeof(desktop_buttons));
	memset(desktop_char_markers, 0, sizeof(desktop_char_markers));
	memset(game_repeat_scans, 0, sizeof(game_repeat_scans));
	desktop_held = false;
	// Registrations still lifted belong to a window destroyed during a capture
	raw_checked = false;
	bool keyboard, mouse;
	restore_raw_registrations(&keyboard, &mouse);
}

// Render thread: puts back the clip that ClipCursor(NULL) released during the
// capture
void restore_clip()
{
	if (!clip_saved)
		return;
	clip_saved = false;
	RECT screen;
	screen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	screen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
	screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
	ClipCursor(EqualRect(&saved_clip, &screen) ? NULL : &saved_clip);
}

// Has the window's thread show or restore the cursor as the capture state says
void post_cursor()
{
	HWND hwnd = hooked_window.load();
	if (hwnd && cursor_message && !cursor_posted.exchange(true) && !PostMessageW(hwnd, cursor_message, 0, 0))
		cursor_posted = false;
}

void send_input(OverlayInput &input, uint32_t connection)
{
	input.mods = current_mods();
	if (!DesktopChannelSendInput(input, connection))
		input_lost = connection;
}

// Window's thread. While both Shift keys are held, Windows sends no WM_KEYUP
// for the one released first. At a key message of the other Shift, a Shift the
// desktop received that the key state reports up is released, to the client
// too during a capture (connection not 0).
void release_missed_shift(WPARAM wp, LPARAM lp, uint32_t connection)
{
	if (wp != VK_SHIFT)
		return;
	static const struct { unsigned scan; int vk; } sides[] = { { 0x2a, VK_LSHIFT }, { 0x36, VK_RSHIFT } };
	unsigned key = key_index(wp, lp);
	bool released = false;
	for (const auto &side : sides) {
		if (side.scan == key || !desktop_keys[side.scan] || GetKeyState(side.vk) < 0)
			continue;
		desktop_keys[side.scan] = false;
		released = true;
		if (connection) {
			OverlayInput input;
			input.kind = INPUT_KEY;
			input.vk = VK_SHIFT;
			input.scan = (uint16_t)side.scan;
			input.down = false;
			send_input(input, connection);
		}
	}
	if (released)
		note_desktop_held();
}

// Window's thread: the window lost activation or focus
void end_capture_from_window(uint32_t connection)
{
	forget_desktop_input();
	AcquireSRWLockExclusive(&capture_lock);
	bool ended = capture_connection.compare_exchange_strong(connection, 0);
	if (ended) {
		OverlayInput input;
		input.kind = INPUT_CAPTURE_ENDED;
		send_input(input, connection);
	}
	ReleaseSRWLockExclusive(&capture_lock);
	if (!ended)
		return;
	unshow_cursor();
	release_mouse();
}

bool mouse_button(UINT msg, WPARAM wp, uint8_t *button, bool *down)
{
	switch (msg) {
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: *button = BUTTON_LEFT; *down = true; return true;
	case WM_LBUTTONUP: *button = BUTTON_LEFT; *down = false; return true;
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: *button = BUTTON_RIGHT; *down = true; return true;
	case WM_RBUTTONUP: *button = BUTTON_RIGHT; *down = false; return true;
	case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: *button = BUTTON_MIDDLE; *down = true; return true;
	case WM_MBUTTONUP: *button = BUTTON_MIDDLE; *down = false; return true;
	case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
		*button = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? BUTTON_BACK : BUTTON_FORWARD;
		*down = msg != WM_XBUTTONUP;
		return true;
	}
	return false;
}

// True when the message is kept from the game, with its result
bool capture_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, uint32_t connection, LRESULT *result)
{
	OverlayInput input;
	*result = 0;
	unsigned scan = (lp >> 16) & 0x1ff;

	if (mouse_button(msg, wp, &input.button, &input.down)) {
		// The game keeps the release of a button it saw pressed
		if (!input.down && !desktop_buttons[input.button])
			return false;
		desktop_buttons[input.button] = input.down;
		note_desktop_held();
		input.kind = INPUT_BUTTON;
		input.x = GET_X_LPARAM(lp);
		input.y = GET_Y_LPARAM(lp);
		send_input(input, connection);
		// The button-up of a press reaches the window even outside it
		if (input.down) {
			SetCapture(hwnd);
			mouse_captured = true;
		} else if (!desktop_button_held()) {
			// Not the MK_ flags: a button the game saw pressed is released to the game
			release_mouse();
		}
		if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK || msg == WM_XBUTTONUP)
			*result = TRUE;
		return true;
	}

	switch (msg) {
	case WM_MOUSEMOVE:
		input.kind = INPUT_MOVE;
		input.x = GET_X_LPARAM(lp);
		input.y = GET_Y_LPARAM(lp);
		send_input(input, connection);
		return true;
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL: {
		POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		CursorUpscalingBypass_ScreenToClient(hwnd, &pt);
		input.kind = INPUT_WHEEL;
		input.x = pt.x;
		input.y = pt.y;
		input.delta = GET_WHEEL_DELTA_WPARAM(wp);
		input.button = msg == WM_MOUSEHWHEEL ? 1 : 0;
		send_input(input, connection);
		return true;
	}
	case WM_SYSKEYDOWN:
		// Alt+F4 still closes the game
		if (wp == VK_F4)
			return false;
		// fall through
	case WM_KEYDOWN:
	case WM_KEYUP:
	case WM_SYSKEYUP: {
		release_missed_shift(wp, lp, connection);
		input.kind = INPUT_KEY;
		input.vk = (uint16_t)wp;
		input.scan = (uint16_t)(((lp >> 16) & 0xff) | (lp & (1 << 24) ? 0xe000 : 0));
		input.down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
		unsigned key = key_index(wp, lp);
		// A key held since before the capture: the game keeps its release,
		// and its repeats and their characters reach neither side
		if (!desktop_keys[key]) {
			if (!input.down)
				return false;
			if (lp & (1 << 30)) {
				game_repeat_scans[scan] = true;
				return true;
			}
		}
		// Kept past the release, which a message loop may handle before the
		// characters of the last repeat: the next press ends it
		if (input.down)
			game_repeat_scans[scan] = false;
		desktop_keys[key] = input.down;
		// Its characters may arrive after the capture ended
		if (input.down)
			post_chars_done(hwnd, scan);
		note_desktop_held();
		send_input(input, connection);
		return true;
	}
	case WM_CHAR:
	case WM_SYSCHAR: {
		if (game_repeat_scans[scan])
			return true;
		WCHAR c = (WCHAR)wp;
		if (IS_HIGH_SURROGATE(c)) {
			high_surrogate = c;
			return true;
		}
		input.rune = c;
		if (IS_LOW_SURROGATE(c)) {
			if (!high_surrogate)
				return true;
			input.rune = 0x10000 + (((uint32_t)high_surrogate - 0xd800) << 10) + (c - 0xdc00);
		}
		high_surrogate = 0;
		input.kind = INPUT_CHAR;
		send_input(input, connection);
		return true;
	}
	case WM_DEADCHAR:
	case WM_SYSDEADCHAR:
		return true;
	case WM_INPUT:
		// The game keeps the release of a key or button it saw pressed, also
		// when it reads raw input only
		if (raw_release_for_game(lp))
			return false;
		// DefWindowProc frees what the raw input message holds
		*result = DefWindowProcW(hwnd, msg, wp, lp);
		return true;
	case WM_SETCURSOR:
		cursor_in_client = LOWORD(lp) == HTCLIENT;
		if (LOWORD(lp) != HTCLIENT)
			return false;
		set_cursor();
		*result = TRUE;
		return true;
	case WM_ACTIVATE:
		if (LOWORD(wp) == WA_INACTIVE)
			end_capture_from_window(connection);
		return false;
	case WM_ACTIVATEAPP:
		if (!wp)
			end_capture_from_window(connection);
		return false;
	case WM_KILLFOCUS:
		end_capture_from_window(connection);
		return false;
	}
	return false;
}

// Outside a capture: true when the message is the release, a repeat or a
// character of a press that went to the desktop, which the game never saw
bool keep_desktop_release(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
	*result = 0;
	unsigned scan = (lp >> 16) & 0x1ff;
	uint8_t button;
	bool down;
	if (mouse_button(msg, wp, &button, &down)) {
		if (!desktop_buttons[button])
			return false;
		desktop_buttons[button] = false;
		note_desktop_held();
		if (!desktop_button_held())
			release_mouse();
		// A press after a release this window did not receive is the game's
		if (down)
			return false;
		if (msg == WM_XBUTTONUP)
			*result = TRUE;
		return true;
	}

	unsigned key = key_index(wp, lp);
	switch (msg) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		release_missed_shift(wp, lp, 0);
		if (lp & (1 << 30)) {
			if (!desktop_keys[key])
				return false;
			post_chars_done(hwnd, scan);
			return true;
		}
		// A press after a release this window did not receive is the game's
		if (desktop_keys[key]) {
			desktop_keys[key] = false;
			note_desktop_held();
		}
		return false;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		release_missed_shift(wp, lp, 0);
		if (!desktop_keys[key])
			return false;
		desktop_keys[key] = false;
		note_desktop_held();
		return true;
	case WM_CHAR:
	case WM_SYSCHAR:
	case WM_DEADCHAR:
	case WM_SYSDEADCHAR:
		return desktop_char_markers[scan] != 0;
	case WM_ACTIVATE:
		if (LOWORD(wp) == WA_INACTIVE)
			forget_desktop_input();
		return false;
	case WM_ACTIVATEAPP:
		if (!wp)
			forget_desktop_input();
		return false;
	case WM_KILLFOCUS:
		forget_desktop_input();
		return false;
	}
	return false;
}

LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	WNDPROC original = (WNDPROC)GetPropW(hwnd, PROP_ORIGINAL);

	if (hwnd == hooked_window.load()) {
		sync_hook_generation();
		uint32_t connection = capture_connection.load();
		sync_nolegacy(connection && connection == DesktopChannelConnection());
	}

	if (cursor_message && msg == cursor_message) {
		cursor_posted = false;
		if (capture_connection.load()) {
			show_cursor();
		} else {
			unshow_cursor();
			// The release of a button the desktop received reaches the
			// window even outside it
			if (!desktop_button_held())
				release_mouse();
		}
		return 0;
	}
	if (chars_done_message && msg == chars_done_message) {
		if (hwnd == hooked_window.load()) {
			sync_hook_generation();
			chars_done(wp, lp);
		}
		return 0;
	}

	if (hwnd == hooked_window.load()) {
		sync_hook_generation();
		LRESULT result;
		uint32_t connection = capture_connection.load();
		if (connection && connection == DesktopChannelConnection()) {
			if (capture_message(hwnd, msg, wp, lp, connection, &result))
				return result;
		} else if (keep_desktop_release(hwnd, msg, wp, lp, &result)) {
			return result;
		}
	}
	return original ? CallWindowProcW(original, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void try_unhook();

bool hook_window()
{
	if (!window || !IsWindow(window))
		return false;
	HWND previous_window = hooked_window.load();
	if (previous_window == window)
		return true;
	// The cursor, the mouse capture and the held keys are recorded for one
	// window's thread only: the previous window has to give them back first
	if (previous_window) {
		try_unhook();
		if (hooked_window.load()) {
			LogInfo("Desktop overlay: window %p still restores its input, capture of %p refused\n",
				previous_window, window);
			return false;
		}
	}
	if (!cursor_message)
		cursor_message = RegisterWindowMessageW(L"NahidaOverlayCursor");
	if (!chars_done_message)
		chars_done_message = RegisterWindowMessageW(L"NahidaOverlayCharsDone");

	// Still in the chain from an earlier capture, under a subclass made after
	// ours: installing again would make the chain call itself
	if (!GetPropW(window, PROP_HOOKED)) {
		LONG_PTR current = GetWindowLongPtrW(window, GWLP_WNDPROC);
		if (!current || !SetPropW(window, PROP_ORIGINAL, (HANDLE)current) || !SetPropW(window, PROP_HOOKED, (HANDLE)1)) {
			LogInfo("Desktop overlay: cannot subclass window %p: %lu\n", window, GetLastError());
			return false;
		}
		SetLastError(0);
		LONG_PTR previous = SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)overlay_proc);
		if (!previous && GetLastError()) {
			LogInfo("Desktop overlay: cannot subclass window %p: %lu\n", window, GetLastError());
			RemovePropW(window, PROP_HOOKED);
			return false;
		}
		if (previous != current)
			SetPropW(window, PROP_ORIGINAL, (HANDLE)previous);
	}
	// Before hooked_window, which the window's thread reads first
	hook_generation++;
	hooked_window = window;
	return true;
}

void unhook_window()
{
	HWND hwnd = hooked_window.load();
	hooked_window = NULL;
	// A cursor message still queued now reaches the game's procedure instead
	cursor_posted = false;
	if (!hwnd || !IsWindow(hwnd))
		return;
	// Under a subclass made after ours, overlay_proc stays in the chain and
	// passes everything on
	if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) != (LONG_PTR)overlay_proc)
		return;
	SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)GetPropW(hwnd, PROP_ORIGINAL));
	RemovePropW(hwnd, PROP_HOOKED);
}

// Removes the subclass once the capture ended, the window's thread gave back
// the cursor, the mouse capture and the raw input registrations, and the keys
// and buttons the desktop received were released
void try_unhook()
{
	HWND hwnd = hooked_window.load();
	if (IsWindow(hwnd)) {
		if (cursor_set.load() || mouse_captured.load() || raw_lifted.load()) {
			post_cursor();
			return;
		}
		if (desktop_held.load())
			return;
	}
	unhook_window();
}

// False if no capture was active
bool end_capture(bool notify, uint32_t connection)
{
	uint32_t captured = capture_connection.exchange(0);
	if (!captured)
		return false;
	// Input the window's thread is still handling must not follow capture ended
	DesktopChannelSetInputConnection(0);
	post_cursor();
	if (notify && captured == connection) {
		OverlayInput input;
		input.kind = INPUT_CAPTURE_ENDED;
		DesktopChannelQueueMessage(DesktopSend::INPUT, MessageType::OverlayInput, 0, input, connection);
	}
	return true;
}

} // namespace

void DesktopOverlayNoteFrame(uint64_t seq)
{
	frame_seq = seq;
}

bool DesktopOverlaySurface(OverlaySurface *out)
{
	AcquireSRWLockShared(&surface_lock);
	bool valid = surface_valid;
	*out = surface;
	ReleaseSRWLockShared(&surface_lock);
	return valid;
}

void DesktopOverlayBegin(HackerDevice *hacker_device, HackerContext *hacker_context, HackerSwapChain *swap_chain,
		uint32_t connection)
{
	device = hacker_device->GetPassThroughOrigDevice1();
	context = hacker_context->GetPassThroughOrigContext1();

	// Disconnecting ends the capture and drops the overlay
	if (connection != overlay_connection) {
		if (end_capture(false, overlay_connection))
			restore_clip();
		release_texture();
		overlay_connection = connection;
	}
	uint32_t lost = input_lost.exchange(0);
	if (lost && lost == connection && capture_connection.load() == lost && end_capture(true, lost))
		restore_clip();
	update_surface(swap_chain, connection);
}

void DesktopOverlaySetTexture(const OverlaySetTexture &request, uint32_t request_id, uint32_t connection)
{
	const char *code = "OVERLAY_OPEN_FAILED";
	std::string detail;
	bool ok = true;

	// A failed request leaves no overlay set
	release_texture();
	if (request.kind != OVERLAY_NONE) {
		texture.kind = request.kind;
		texture.width = request.width;
		texture.height = request.height;
		if (!device) {
			ok = false;
			detail = "no device";
		} else if (!request.width || !request.height || request.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
				|| request.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
			ok = false;
			detail = "size " + std::to_string(request.width) + "x" + std::to_string(request.height);
		} else {
			ok = request.kind == OVERLAY_KMT ? open_kmt(request, &code, &detail) : open_section(request, &code, &detail);
			ok = ok && create_copy(&code, &detail);
		}
	}
	if (!ok) {
		LogInfo("Desktop overlay: kind %u handle 0x%x section %s %ux%u: %s %s\n", request.kind, request.handle,
			request.section.c_str(), request.width, request.height, code, detail.c_str());
		release_texture();
		DesktopChannelQueueError(request_id, code, detail, connection);
		return;
	}
	OverlayTextureSet reply;
	DesktopChannelQueueMessage(DesktopSend::REPLY, MessageType::OverlayTextureSet, request_id, reply, connection);
}

void DesktopOverlayCapture(const OverlayCapture &request, uint32_t request_id, uint32_t connection)
{
	OverlayCaptured reply;

	if (!request.capture) {
		if (end_capture(false, connection))
			restore_clip();
		DesktopChannelQueueMessage(DesktopSend::REPLY, MessageType::OverlayCaptured, request_id, reply, connection);
		return;
	}

	capture_cursor = request.cursor;
	bool hooked = hook_window();
	if (!hooked) {
		// A capture still running on the previous window ends with this one
		if (end_capture(false, connection))
			restore_clip();
		DesktopChannelQueueMessage(DesktopSend::REPLY, MessageType::OverlayCaptured, request_id, reply, connection);
		OverlayInput ended;
		ended.kind = INPUT_CAPTURE_ENDED;
		DesktopChannelQueueMessage(DesktopSend::INPUT, MessageType::OverlayInput, 0, ended, connection);
		return;
	}
	// The reply goes after the capture ended of a capture the window's thread
	// is ending, and before that of a capture it ends from here on
	AcquireSRWLockExclusive(&capture_lock);
	DesktopChannelQueueMessage(DesktopSend::REPLY, MessageType::OverlayCaptured, request_id, reply, connection);
	if (!capture_connection.load()) {
		clip_saved = !!GetClipCursor(&saved_clip);
		cursor_in_client = true;
	}
	// The window's thread sends input from here on, after the reply
	DesktopChannelSetInputConnection(connection);
	capture_connection = connection;
	ReleaseSRWLockExclusive(&capture_lock);
	post_cursor();
}

void DesktopOverlayEnd(HackerSwapChain *swap_chain)
{
	if (capture_connection.load()) {
		// The game may clip, hide or replace the cursor during its frame
		ClipCursor(NULL);
		CURSORINFO info = { sizeof(info) };
		if (CursorBypass_GetCursorInfo(&info) && (!(info.flags & CURSOR_SHOWING) || CursorIsHidden(info.hCursor)
				|| (cursor_in_client.load() && info.hCursor != cursor_handle())))
			post_cursor();
	} else if (hooked_window.load()) {
		try_unhook();
	}

	if (!texture.srv)
		return;
	update_copy();
	if (texture.has_content && swap_chain->mOverlay)
		swap_chain->mOverlay->DrawTexture(texture.srv);
}

void DesktopOverlaySwapChainReleased(uint32_t connection)
{
	end_capture(true, connection);
	clip_saved = false;
	release_texture();
	// The window may outlive its swap chain: the subclass stays until the
	// window's thread gave back the cursor
	try_unhook();
	window = NULL;
	device = NULL;
	context = NULL;
	luid_device = NULL;

	AcquireSRWLockExclusive(&surface_lock);
	bool was_valid = surface_valid;
	OverlaySurface gone;
	gone.hwnd = surface.hwnd;
	surface_valid = false;
	ReleaseSRWLockExclusive(&surface_lock);
	if (was_valid && connection)
		DesktopChannelQueueMessage(DesktopSend::EVENT, MessageType::OverlaySurface, 0, gone, connection);
}
