#pragma once

// Stand-ins for DirectX's own context and device, handed to the game for the
// interfaces HackerContext and HackerDevice do not implement (ID3D11DeviceContext2
// and on, ID3D11Device2 and on; IUnknown stays DirectX's own). Every call goes
// straight to DirectX's object, as it would if the game had that object, except
// that what the game binds through the context is noted in, or forgotten from,
// HackerContext::mGameSrvs and mGameVertexBuffers as through HackerContext, and
// the stand-ins hand out each other: the context its device's, the device its
// immediate context's. Only built for x64:
// elsewhere these return NULL and the game gets DirectX's object.
//
// This header does not include the C++ Direct3D headers, as BindingProxy.cpp
// uses the C ones.

#include <unknwn.h>

class HackerContext;
class HackerDevice;

// real is the context of the HackerContext; the stand-in passes calls to the
// newest context interface it has. NULL if none can be made.
IUnknown* MakeContextBindingProxy(IUnknown *real, HackerContext *context);
// real is the device of the HackerDevice, likewise.
IUnknown* MakeDeviceBindingProxy(IUnknown *real, HackerDevice *device);
// Once nothing can call it any more:
void FreeBindingProxy(IUnknown *proxy);

// Implemented beside HackerContext and HackerDevice for the stand-ins, doing
// what the methods of HackerContext do for the same binds (NoteGameSrvsOf
// also HackerContext::ForgetCommandListSrvs). stage: 0-5 for vs, hs, ds, gs, ps, cs.
struct ID3D11ShaderResourceView;
struct ID3D11Buffer;
void NoteGameSrvsOf(HackerContext *context, int stage, UINT first, UINT count, ID3D11ShaderResourceView *const *views);
void NoteGameVertexBuffersOf(HackerContext *context, UINT first, UINT count, ID3D11Buffer *const *buffers, const UINT *strides, const UINT *offsets);
void ForgetGameBindingsOf(HackerContext *context);
// The stand-in of the device of the context, NULL if the game gets DirectX's
// device from it (device hooks, or none could be made):
IUnknown* DeviceBindingProxyOf(HackerContext *context);
// The stand-in of the device's immediate context, NULL if there is none:
IUnknown* ImmediateContextBindingProxy(HackerDevice *device);
// The game has a context of DirectX's own that it may bind through, see
// game_has_unwrapped_context:
void NoteUnwrappedContext(const char *how);
