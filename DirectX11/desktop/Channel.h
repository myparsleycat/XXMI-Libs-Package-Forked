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

// [Desktop] channel, as read by the first configuration load. Every hook of
// the channel checks this first.
extern bool desktop_channel_enabled;

// The [Desktop] section, from LoadConfigFile()
void ParseDesktopSection();

// At every Present, from the presenting thread
void DesktopChannelFrame(uint8_t render_api);
// After the configuration was reloaded
void DesktopChannelReloaded();
// From DllMain; sends SHUTDOWN when the process is terminating
void DesktopChannelDetach(bool process_terminating);

bool DesktopChannelConnected();
uint32_t DesktopChannelConnection();
// Queues a frame for the client
enum class DesktopSend { REPLY, EVENT };
void DesktopChannelQueue(DesktopSend kind, std::vector<uint8_t> &&frame, uint32_t connection);

std::string DesktopUtf8(const wchar_t *text, size_t length = (size_t)-1);
