#pragma once

// The channel to Nahida Desktop (docs/desktop-channel.md): a named pipe the
// DLL serves from a thread of its own once the game presents, with
// [Desktop] channel = 1. The render thread never waits for the pipe or for
// that thread: what it sends goes through queues of bounded size, and what the
// client asks for that touches 3DMigoto's state is carried out at the next
// Present, in DesktopChannelFrame().

#include <stdint.h>
#include <string>
#include <vector>
#include "Protocol.h"

class HackerDevice;
class HackerContext;
class HackerSwapChain;

// [Desktop] channel, as read by the first configuration load. Every hook of
// the channel checks this first.
extern bool desktop_channel_enabled;

// The [Desktop] section, from LoadConfigFile()
void ParseDesktopSection();

// At every Present, from the presenting thread. hacker_context is the
// device's immediate context, NULL on D3D12 and Vulkan. D3D11 uses swap_chain
// until that chain is released or another one presented many times without
// it; other swap chains do not advance the channel.
void DesktopChannelFrame(HackerDevice *device, HackerContext *hacker_context, uint8_t render_api,
	HackerSwapChain *swap_chain = NULL);
void DesktopChannelSwapChainReleased(HackerSwapChain *swap_chain);
// After the configuration was reloaded
void DesktopChannelReloaded();
// From DllMain; sends SHUTDOWN when the process is terminating
void DesktopChannelDetach(bool process_terminating);

bool DesktopChannelConnected();
uint32_t DesktopChannelConnection();
enum class DesktopSend { REPLY, EVENT, INPUT };
// Queues a frame for the client. Only from code that DesktopChannelFrame(),
// DesktopChannelSwapChainReleased() or DesktopChannelReloaded() runs, which
// hold the lock that guards the queue. INPUT frames are never dropped for
// the limit on events.
void DesktopChannelQueue(DesktopSend kind, std::vector<uint8_t> &&frame, uint32_t connection);
void DesktopChannelEncodeFailed(DesktopProtocol::MessageType type);
template <class M> void DesktopChannelQueueMessage(DesktopSend kind, DesktopProtocol::MessageType type,
	uint32_t request_id, M &message, uint32_t connection)
{
	std::vector<uint8_t> frame;
	if (DesktopProtocol::EncodeFrame(type, request_id, message, &frame))
		DesktopChannelQueue(kind, std::move(frame), connection);
	else
		DesktopChannelEncodeFailed(type);
}
void DesktopChannelQueueError(uint32_t request_id, const char *code, const std::string &detail, uint32_t connection);
// Under the same rule: hands what was queued to the pipe thread, waiting for
// the queue lock, then lets DesktopChannelSendInput() queue input of
// connection only, 0 for none. Input that the window's thread was handling
// when a capture ended is dropped rather than sent after the end.
void DesktopChannelSetInputConnection(uint32_t connection);
// Queues an OverlayInput frame from any thread, behind what is queued already.
// Capture ended from here also closes the input to the connection.
// Waits only for the pipe thread to move the queue. When the client falls
// behind, moves are dropped before other input, and releases and the end of a
// capture are kept. False when a release was dropped all the same: the queue
// holds nothing but releases, and the capture has to end.
bool DesktopChannelSendInput(DesktopProtocol::OverlayInput &input, uint32_t connection);

std::string DesktopUtf8(const wchar_t *text, size_t length = (size_t)-1);
