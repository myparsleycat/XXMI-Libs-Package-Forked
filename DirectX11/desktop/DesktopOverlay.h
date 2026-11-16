#pragma once

// The desktop overlay of the channel on D3D11 (docs/desktop-channel.md,
// Overlay): Nahida Desktop draws its UI into a shared texture or a shared
// memory section, and this draws it over the driving swap chain's back buffer
// at Present, before 3DMigoto's own overlay. While the desktop captures input,
// a subclass of the swap chain's window forwards the window's mouse and
// keyboard messages to the desktop instead of passing them to the game.
//
// Everything but DesktopOverlayNoteFrame() and DesktopOverlaySurface() runs
// under the channel's frame lock, on the thread presenting the driving swap
// chain or releasing it.

#include <stdint.h>
#include "Protocol.h"

class HackerDevice;
class HackerContext;
class HackerSwapChain;

// From the pipe thread, for OverlayFrame
void DesktopOverlayNoteFrame(uint64_t seq);
// From the pipe thread after HelloReply: the surface last reported, false
// while there is no driving swap chain
bool DesktopOverlaySurface(DesktopProtocol::OverlaySurface *surface);

// At a Present of the driving swap chain, before the requests of that Present
void DesktopOverlayBegin(HackerDevice *device, HackerContext *context, HackerSwapChain *swap_chain,
		uint32_t connection);
// Requests, which queue their replies to connection
void DesktopOverlaySetTexture(const DesktopProtocol::OverlaySetTexture &request, uint32_t request_id,
		uint32_t connection);
void DesktopOverlayCapture(const DesktopProtocol::OverlayCapture &request, uint32_t request_id,
		uint32_t connection);
// After the requests: updates the copy of the overlay and draws it
void DesktopOverlayEnd(HackerSwapChain *swap_chain);
// When the driving swap chain is released
void DesktopOverlaySwapChainReleased(uint32_t connection);
