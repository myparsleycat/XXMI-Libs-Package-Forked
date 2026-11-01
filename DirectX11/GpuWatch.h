#pragma once

#include <d3d11_1.h>

#include "DrawCallInfo.h"

// Finds the draw call a D3D11 GPU hang (TDR) is stuck on: a fence is signalled
// after every draw and dispatch, and a thread of its own, which needs neither
// the context nor a working render thread, watches how far the GPU has got.
namespace GpuWatch {

extern bool enabled;
extern bool debug_layer;

void init(int calls_per_signal, bool use_debug_layer, bool use_aftermath, const wchar_t *log_folder);
void frame(ID3D11Device1 *device, ID3D11DeviceContext1 *context);

// NVIDIA Aftermath GPU crash dumps, which give the fault type the fence cannot.
void before_device_create();
void after_device_create(ID3D11Device *device);
void after_present(HRESULT result);

// hashes = vs, hs, ds, gs, ps, or the cs alone for a dispatch
void mark_call(ID3D11DeviceContext *context, const DrawCallInfo &info, const UINT64 hashes[5]);
// hashes = the shaders the game has bound, which a custom shader section may replace
void mark_section(ID3D11DeviceContext *context, const wchar_t *ini_section, const UINT64 hashes[5]);
void mark_command_list(ID3D11DeviceContext *context);

}
