// See BindingProxy.h. The C declarations of the interfaces give the tables as
// structures of function pointers, see HookedContext.cpp for why that rules
// out the C++ headers of 3DMigoto here.

#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d11_4.h>
#undef D3D11_NO_HELPERS
#undef CINTERFACE

#include <new>
#include <stdint.h>

#include "BindingProxy.h"

#ifdef _WIN64

// Every entry of a table is a few bytes of code that pass the call on to the
// same entry of the real object, whatever its arguments, reading the table of
// that object at the time of the call (Direct3D rewrites entries of its
// tables as it goes). The entries listened to are replaced by the functions
// below.
struct BindingProxy {
	void **table;
	IUnknown *real; // At 8, where the code of the table reads it
	IUnknown *identity;
	HackerContext *context;
	HackerDevice *device;
};

static const int TABLE_SLOTS = 256;
static_assert(sizeof(ID3D11DeviceContext4Vtbl) <= TABLE_SLOTS * sizeof(void*), "context table");
static_assert(sizeof(ID3D11Device5Vtbl) <= TABLE_SLOTS * sizeof(void*), "device table");
static void *context_table[TABLE_SLOTS], *device_table[TABLE_SLOTS];
static INIT_ONCE tables_once = INIT_ONCE_STATIC_INIT;
static bool tables_made;

// Whatever interface is the object itself is the stand-in, the others (DXGI's,
// the multithread switch) are the real ones. So is IUnknown, which keeps the
// COM identity of DirectX's object, as HackerDevice and HackerContext do:
static HRESULT STDMETHODCALLTYPE proxy_QueryInterface(BindingProxy *self, REFIID riid, void **out)
{
	HRESULT hr = self->real->lpVtbl->QueryInterface(self->real, riid, out);
	if (SUCCEEDED(hr) && out && !IsEqualIID(riid, IID_IUnknown) && (*out == self->real || *out == self->identity))
		*out = self;
	return hr;
}

#define SRV_SETTER(stage, slots) \
static void STDMETHODCALLTYPE proxy_##stage##SetShaderResources(BindingProxy *self, \
		UINT first, UINT count, ID3D11ShaderResourceView *const *views) \
{ \
	ID3D11DeviceContext *real = (ID3D11DeviceContext*)self->real; \
	real->lpVtbl->stage##SetShaderResources(real, first, count, views); \
	NoteGameSrvsOf(self->context, slots, first, count, views); \
}
SRV_SETTER(VS, 0)
SRV_SETTER(HS, 1)
SRV_SETTER(DS, 2)
SRV_SETTER(GS, 3)
SRV_SETTER(PS, 4)
SRV_SETTER(CS, 5)

static void STDMETHODCALLTYPE proxy_IASetVertexBuffers(BindingProxy *self, UINT first, UINT count,
		ID3D11Buffer *const *buffers, const UINT *strides, const UINT *offsets)
{
	ID3D11DeviceContext *real = (ID3D11DeviceContext*)self->real;
	real->lpVtbl->IASetVertexBuffers(real, first, count, buffers, strides, offsets);
	NoteGameVertexBuffersOf(self->context, first, count, buffers, strides, offsets);
}

static void STDMETHODCALLTYPE proxy_ClearState(BindingProxy *self)
{
	ID3D11DeviceContext *real = (ID3D11DeviceContext*)self->real;
	real->lpVtbl->ClearState(real);
	ForgetGameBindingsOf(self->context);
}

static void STDMETHODCALLTYPE proxy_ExecuteCommandList(BindingProxy *self, ID3D11CommandList *list, BOOL restore)
{
	ID3D11DeviceContext *real = (ID3D11DeviceContext*)self->real;
	real->lpVtbl->ExecuteCommandList(real, list, restore);
	ForgetGameBindingsOf(self->context);
}

static void STDMETHODCALLTYPE proxy_SwapDeviceContextState(BindingProxy *self, ID3DDeviceContextState *state, ID3DDeviceContextState **previous)
{
	ID3D11DeviceContext1 *real = (ID3D11DeviceContext1*)self->real;
	real->lpVtbl->SwapDeviceContextState(real, state, previous);
	ForgetGameBindingsOf(self->context);
}

static void STDMETHODCALLTYPE proxy_GetDevice(BindingProxy *self, ID3D11Device **out)
{
	ID3D11DeviceContext *real = (ID3D11DeviceContext*)self->real;
	real->lpVtbl->GetDevice(real, out);
	// The reference the call took on the device is the stand-in's:
	if (out && *out) {
		if (IUnknown *device = DeviceBindingProxyOf(self->context))
			*out = (ID3D11Device*)device;
	}
}

static void use_immediate_context_proxy(BindingProxy *self, void **out)
{
	if (!*out)
		return;

	BindingProxy *context = (BindingProxy*)ImmediateContextBindingProxy(self->device);
	IUnknown *got = (IUnknown*)*out, *identity = NULL;
	if (SUCCEEDED(got->lpVtbl->QueryInterface(got, IID_IUnknown, (void**)&identity)))
		identity->lpVtbl->Release(identity);
	// The reference the call took on the context is the stand-in's:
	if (context && identity && identity == context->identity)
		*out = context;
	else
		NoteUnwrappedContext("the immediate context of an ID3D11Device2 or later");
}

static void STDMETHODCALLTYPE proxy_GetImmediateContext(BindingProxy *self, ID3D11DeviceContext **out)
{
	ID3D11Device *real = (ID3D11Device*)self->real;
	real->lpVtbl->GetImmediateContext(real, out);
	if (out)
		use_immediate_context_proxy(self, (void**)out);
}

static void STDMETHODCALLTYPE proxy_GetImmediateContext1(BindingProxy *self, ID3D11DeviceContext1 **out)
{
	ID3D11Device1 *real = (ID3D11Device1*)self->real;
	real->lpVtbl->GetImmediateContext1(real, out);
	if (out)
		use_immediate_context_proxy(self, (void**)out);
}

static void STDMETHODCALLTYPE proxy_GetImmediateContext2(BindingProxy *self, ID3D11DeviceContext2 **out)
{
	ID3D11Device2 *real = (ID3D11Device2*)self->real;
	real->lpVtbl->GetImmediateContext2(real, out);
	if (out)
		use_immediate_context_proxy(self, (void**)out);
}

static void STDMETHODCALLTYPE proxy_GetImmediateContext3(BindingProxy *self, ID3D11DeviceContext3 **out)
{
	ID3D11Device3 *real = (ID3D11Device3*)self->real;
	real->lpVtbl->GetImmediateContext3(real, out);
	if (out)
		use_immediate_context_proxy(self, (void**)out);
}

static BOOL CALLBACK make_tables(PINIT_ONCE, PVOID, PVOID*)
{
	const SIZE_T size = TABLE_SLOTS * 16;
	uint8_t *code = (uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!code)
		return TRUE;
	for (int slot = 0; slot < TABLE_SLOTS; slot++) {
		// mov rcx, [rcx + 8]; mov rax, [rcx]; jmp [rax + slot * 8]
		uint8_t thunk[16] = { 0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0xA0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC };
		int32_t offset = slot * 8;
		memcpy(thunk + 9, &offset, 4);
		memcpy(code + slot * 16, thunk, 16);
		context_table[slot] = device_table[slot] = code + slot * 16;
	}
	DWORD old;
	if (!VirtualProtect(code, size, PAGE_EXECUTE_READ, &old)) {
		VirtualFree(code, 0, MEM_RELEASE);
		return TRUE;
	}
	FlushInstructionCache(GetCurrentProcess(), code, size);

	ID3D11DeviceContext4Vtbl *c = (ID3D11DeviceContext4Vtbl*)context_table;
#define C(name, function) *(void**)&c->name = (void*)function;
	C(QueryInterface, proxy_QueryInterface)
	C(GetDevice, proxy_GetDevice)
	C(VSSetShaderResources, proxy_VSSetShaderResources)
	C(HSSetShaderResources, proxy_HSSetShaderResources)
	C(DSSetShaderResources, proxy_DSSetShaderResources)
	C(GSSetShaderResources, proxy_GSSetShaderResources)
	C(PSSetShaderResources, proxy_PSSetShaderResources)
	C(CSSetShaderResources, proxy_CSSetShaderResources)
	C(IASetVertexBuffers, proxy_IASetVertexBuffers)
	C(ClearState, proxy_ClearState)
	C(ExecuteCommandList, proxy_ExecuteCommandList)
	C(SwapDeviceContextState, proxy_SwapDeviceContextState)
#undef C

	ID3D11Device5Vtbl *d = (ID3D11Device5Vtbl*)device_table;
#define D(name, function) *(void**)&d->name = (void*)function;
	D(QueryInterface, proxy_QueryInterface)
	D(GetImmediateContext, proxy_GetImmediateContext)
	D(GetImmediateContext1, proxy_GetImmediateContext1)
	D(GetImmediateContext2, proxy_GetImmediateContext2)
	D(GetImmediateContext3, proxy_GetImmediateContext3)
#undef D

	tables_made = true;
	return TRUE;
}

// The newest of the interfaces that the object has, without a reference: the
// stand-in is only called while the caller holds one on the object.
static IUnknown* newest_interface(IUnknown *real, const IID *const *iids, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		IUnknown *got = NULL;
		if (SUCCEEDED(real->lpVtbl->QueryInterface(real, *iids[i], (void**)&got))) {
			got->lpVtbl->Release(got);
			return got;
		}
	}
	return real;
}

static BindingProxy* make_proxy(IUnknown *real, void **table, const IID *const *iids, size_t count)
{
	InitOnceExecuteOnce(&tables_once, make_tables, NULL, NULL);
	if (!tables_made || !real)
		return NULL;

	IUnknown *identity = NULL;
	if (FAILED(real->lpVtbl->QueryInterface(real, IID_IUnknown, (void**)&identity)))
		return NULL;
	identity->lpVtbl->Release(identity);

	BindingProxy *proxy = new (std::nothrow) BindingProxy();
	if (!proxy)
		return NULL;
	proxy->table = table;
	proxy->real = newest_interface(real, iids, count);
	proxy->identity = identity;
	return proxy;
}

IUnknown* MakeContextBindingProxy(IUnknown *real, HackerContext *context)
{
	static const IID *const iids[] = { &IID_ID3D11DeviceContext4, &IID_ID3D11DeviceContext3, &IID_ID3D11DeviceContext2 };
	BindingProxy *proxy = make_proxy(real, context_table, iids, ARRAYSIZE(iids));
	if (proxy)
		proxy->context = context;
	return (IUnknown*)proxy;
}

IUnknown* MakeDeviceBindingProxy(IUnknown *real, HackerDevice *device)
{
	static const IID *const iids[] = { &IID_ID3D11Device5, &IID_ID3D11Device4, &IID_ID3D11Device3, &IID_ID3D11Device2 };
	BindingProxy *proxy = make_proxy(real, device_table, iids, ARRAYSIZE(iids));
	if (proxy)
		proxy->device = device;
	return (IUnknown*)proxy;
}

void FreeBindingProxy(IUnknown *proxy)
{
	delete (BindingProxy*)proxy;
}

#else

IUnknown* MakeContextBindingProxy(IUnknown *real, HackerContext *context)
{
	return NULL;
}

IUnknown* MakeDeviceBindingProxy(IUnknown *real, HackerDevice *device)
{
	return NULL;
}

void FreeBindingProxy(IUnknown *proxy)
{
}

#endif
