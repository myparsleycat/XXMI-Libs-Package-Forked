#pragma once

#include <d3d12.h>

// What DRED (see enable_dred in Dx12Hooks.cpp) kept of a removed device,
// through note: the command lists the GPU had not got through, and what was at
// the address of a page fault, of the mods and of the game
void Dx12NoteDred(ID3D12Device *device, void (*note)(const char *format, ...));

// With dx12_dred or dx12_stall_dump_seconds, for the device of the shim: a
// thread of its own reports the removal of the device, and writes a minidump
// once nothing has been presented for that many seconds with the game's
// window in front. Called again after a reload of the configuration, it starts
// the thread if it is not running yet and a key is set now.
void Dx12WatchDevice(ID3D12Device *device);
