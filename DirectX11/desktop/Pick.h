#pragma once

// Picking for the desktop channel on D3D11: what the draws of one frame put at
// the cursor pixel. Each draw that can be picked is probed right before
// the game's (or a command list's) draw with the same shaders before the
// rasterizer and a pixel shader of ours in place of the game's, once with depth
// testing off and once with its depth and stencil tests, both without writes
// to render targets, depth or stencil.
// That shader writes what reaches the cursor pixel to a buffer we read back a
// few frames later.

#include <d3d11_1.h>
#include <stdint.h>
#include <string>

class HackerDevice;
class HackerContext;
class CommandListState;
struct DrawContext;
struct DrawCallInfo;

extern bool desktop_channel_enabled;
// True from the Present that starts a capture until the next one. The draw
// hooks below are called only while it is set.
extern volatile bool desktop_pick_capturing;

// From the render thread at a Present. 0 when a pick is in progress.
uint64_t DesktopPickArm(uint8_t trigger, bool cursor_given, float x, float y, uint32_t connection);
// At every Present of the driving swap chain, after DesktopPickArm() may have run
void DesktopPickFrame(HackerDevice *device, HackerContext *context, HWND window);
void DesktopPickCancel();

// Snapshot before command lists, probe after pre command lists, then end after
// post command lists.
void DesktopPickBeforeDraw(HackerContext *context, DrawContext &data);
void DesktopPickGameDraw(HackerContext *context, DrawContext &data);
void DesktopPickAfterDrawEnd(HackerContext *context);
void DesktopPickExecuteCommandList(HackerContext *context);

// A draw a command list made, with its arguments as the docs number the calls
// (DesktopProtocol::DrawCallType), call 0 for one that cannot be redrawn
struct DesktopPickDraw {
	uint8_t call;
	UINT count, instances, start;
	INT base_vertex;
	UINT start_instance;
	ID3D11Buffer *args;
	UINT args_offset;
};
// The draw a DrawCallInfo describes
DesktopPickDraw DesktopPickDrawOf(const DrawCallInfo &info);
void DesktopPickCommandDraw(CommandListState *state, const std::wstring &section, const std::wstring &ini_line,
		const DesktopPickDraw &draw);
// A [TextureOverride] (OVERRIDE_TEXTURE) or [ShaderOverride] whose command
// list runs for the game's draw
void DesktopPickNoteOverride(HackerContext *context, uint8_t kind, const std::wstring &section);

// Every shader the DLL creates, with the bytecode it was made from, so that a
// pick knows which output register holds SV_Position. replaced is set for a
// ShaderFixes, ShaderRegex or reloaded shader that takes the game's place. A
// handle that is reused is noted again by whichever of these creates it.
void DesktopNoteShader(ID3D11DeviceChild *shader, const void *bytecode, SIZE_T size, bool replaced);
