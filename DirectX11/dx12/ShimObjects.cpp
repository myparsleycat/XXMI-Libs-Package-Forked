#include "ShimObjects.h"

#include <algorithm>

#include "ShimDevice.h"
#include "ShimPipeline.h"
#include "../vk/VkScope.h"

UINT64 ShimNextId()
{
	static UINT64 id;

	return ++id;
}

void Dx12ShimGetDevice(ID3D11Device **ppDevice)
{
	dx12_shim->shim_device->AddRef();
	*ppDevice = dx12_shim->shim_device;
}

HRESULT ShimQueryInterface(IUnknown *object, const IID *const *iids, size_t count, REFIID riid, void **ppvObject)
{
	size_t i;

	if (!ppvObject)
		return E_POINTER;

	for (i = 0; i < count; i++) {
		if (IsEqualIID(*iids[i], riid)) {
			object->AddRef();
			*ppvObject = object;
			return S_OK;
		}
	}

	*ppvObject = NULL;
	return E_NOINTERFACE;
}

// -----------------------------------------------------------------------------

ShimPrivateData::~ShimPrivateData()
{
	// An interface may reach back to us while it is being released
	while (!mEntries.empty()) {
		IUnknown *unknown = mEntries.back().unknown;

		mEntries.pop_back();
		if (unknown)
			unknown->Release();
	}
}

void ShimPrivateData::Remove(REFGUID guid)
{
	size_t i;

	for (i = 0; i < mEntries.size(); i++) {
		if (IsEqualGUID(mEntries[i].guid, guid)) {
			IUnknown *unknown = mEntries[i].unknown;

			mEntries.erase(mEntries.begin() + i);
			if (unknown)
				unknown->Release();
			return;
		}
	}
}

HRESULT ShimPrivateData::Get(REFGUID guid, UINT *pDataSize, void *pData)
{
	if (!pDataSize)
		return E_INVALIDARG;

	for (Entry &entry : mEntries) {
		UINT size;

		if (!IsEqualGUID(entry.guid, guid))
			continue;

		size = entry.unknown ? sizeof(IUnknown*) : (UINT)entry.data.size();
		if (!pData) {
			*pDataSize = size;
			return S_OK;
		}
		if (*pDataSize < size) {
			*pDataSize = size;
			return DXGI_ERROR_MORE_DATA;
		}

		*pDataSize = size;
		if (entry.unknown) {
			entry.unknown->AddRef();
			*(IUnknown**)pData = entry.unknown;
		} else {
			memcpy(pData, entry.data.data(), size);
		}
		return S_OK;
	}

	*pDataSize = 0;
	return DXGI_ERROR_NOT_FOUND;
}

HRESULT ShimPrivateData::Set(REFGUID guid, UINT DataSize, const void *pData)
{
	Remove(guid);
	if (!pData)
		return S_OK;

	mEntries.push_back({ guid, NULL, std::vector<BYTE>((const BYTE*)pData, (const BYTE*)pData + DataSize) });
	return S_OK;
}

HRESULT ShimPrivateData::SetInterface(REFGUID guid, const IUnknown *pData)
{
	IUnknown *unknown = const_cast<IUnknown*>(pData);

	Remove(guid);
	if (!unknown)
		return S_OK;

	unknown->AddRef();
	mEntries.push_back({ guid, unknown });
	return S_OK;
}

// -----------------------------------------------------------------------------

void ShimDestroyResource(ShimResource *resource)
{
	if (resource->staged) {
		bool counted = resource->staged->counted;

		Dx12ShimRecycleStagedUpload(resource->staged->upload, resource->staged->copy_fence);
		delete resource->staged;
		resource->staged = nullptr;
		// The loader can destroy a resource that has never reached present.
		if (!counted) {
			if (resource->resource)
				resource->resource->Release();
			resource->resource = NULL;
			return;
		}
	}
	Dx12ShimForgetResource(resource);
	if (resource->mirrored)
		VkScopeForgetResource(resource);

	if (resource->resource && !resource->foreign) {
		Dx12ShimDeferRelease(resource->resource);
		resource->resource->Release();
	}
	if (resource->allocated)
		ShimCountResource(resource, -1);
	if (resource->readback) {
		Dx12ShimDeferRelease(resource->readback);
		resource->readback->Release();
	}
	// The lists using it have their own references
	if (resource->game_upload)
		resource->game_upload->Release();
	ShimForgetSpares(resource);
}

void ShimDestroyView(ShimView *view)
{
	Dx12ShimFreeDescriptor(view->heap_type, view->handle);
	// A view made up for a resource of the game (new_game_view) has no
	// descriptor, and is not among its resource's
	if (view->resource) {
		std::vector<ShimView*> &views = view->resource->views;
		auto found = std::find(views.begin(), views.end(), view);

		if (found != views.end())
			views.erase(found);
	}
	if (view->resource11)
		view->resource11->Release();
}

UINT ShimTextureArraySize(const D3D12_RESOURCE_DESC &desc)
{
	return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
}

UINT ShimPlaneCount(ShimResource *resource)
{
	if (resource->dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
		return 1;
	if (!resource->plane_count) {
		D3D12_FEATURE_DATA_FORMAT_INFO info = {resource->desc.Format};
		resource->plane_count = SUCCEEDED(dx12_shim->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)))
			&& info.PlaneCount ? info.PlaneCount : 1;
	}
	return resource->plane_count;
}

template <class View>
static void array_range(ShimView::Range &range, const View &view)
{
	range.slice = view.FirstArraySlice;
	range.slices = view.ArraySize;
}

template <class View>
static void mip_range(ShimView::Range &range, const View &view)
{
	range.mip = view.MostDetailedMip;
	range.mips = view.MipLevels;
}

void ShimSetViewRange(ShimView *view)
{
	auto &range = view->range;
	const auto &desc = view->resource->desc;

	switch (view->kind) {
	case ShimView::SRV: {
		const auto &srv = view->desc12.srv;
		switch (srv.ViewDimension) {
		case D3D12_SRV_DIMENSION_TEXTURE1D: mip_range(range, srv.Texture1D); break;
		case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: mip_range(range, srv.Texture1DArray); array_range(range, srv.Texture1DArray); break;
		case D3D12_SRV_DIMENSION_TEXTURE2D: mip_range(range, srv.Texture2D); break;
		case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: mip_range(range, srv.Texture2DArray); array_range(range, srv.Texture2DArray); break;
		case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: array_range(range, srv.Texture2DMSArray); break;
		case D3D12_SRV_DIMENSION_TEXTURE3D: mip_range(range, srv.Texture3D); break;
		case D3D12_SRV_DIMENSION_TEXTURECUBE: mip_range(range, srv.TextureCube); range.slices = 6; break;
		case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
			mip_range(range, srv.TextureCubeArray);
			range.slice = srv.TextureCubeArray.First2DArrayFace;
			range.slices = srv.TextureCubeArray.NumCubes == UINT_MAX ? UINT_MAX : srv.TextureCubeArray.NumCubes * 6;
			break;
		}
		if (srv.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || srv.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT)
			range.planes = 2;
		break;
	}
	case ShimView::UAV: {
		const auto &uav = view->desc12.uav;
		switch (uav.ViewDimension) {
		case D3D12_UAV_DIMENSION_TEXTURE1D: range.mip = uav.Texture1D.MipSlice; break;
		case D3D12_UAV_DIMENSION_TEXTURE1DARRAY: range.mip = uav.Texture1DArray.MipSlice; array_range(range, uav.Texture1DArray); break;
		case D3D12_UAV_DIMENSION_TEXTURE2D: range.mip = uav.Texture2D.MipSlice; break;
		case D3D12_UAV_DIMENSION_TEXTURE2DARRAY: range.mip = uav.Texture2DArray.MipSlice; array_range(range, uav.Texture2DArray); break;
		case D3D12_UAV_DIMENSION_TEXTURE3D: range.mip = uav.Texture3D.MipSlice; break;
		}
		break;
	}
	case ShimView::RTV: {
		const auto &rtv = view->desc12.rtv;
		switch (rtv.ViewDimension) {
		case D3D12_RTV_DIMENSION_TEXTURE1D: range.mip = rtv.Texture1D.MipSlice; break;
		case D3D12_RTV_DIMENSION_TEXTURE1DARRAY: range.mip = rtv.Texture1DArray.MipSlice; array_range(range, rtv.Texture1DArray); break;
		case D3D12_RTV_DIMENSION_TEXTURE2D: range.mip = rtv.Texture2D.MipSlice; break;
		case D3D12_RTV_DIMENSION_TEXTURE2DARRAY: range.mip = rtv.Texture2DArray.MipSlice; array_range(range, rtv.Texture2DArray); break;
		case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY: array_range(range, rtv.Texture2DMSArray); break;
		case D3D12_RTV_DIMENSION_TEXTURE3D: range.mip = rtv.Texture3D.MipSlice; break;
		}
		break;
	}
	case ShimView::DSV: {
		const auto &dsv = view->desc12.dsv;
		switch (dsv.ViewDimension) {
		case D3D12_DSV_DIMENSION_TEXTURE1D: range.mip = dsv.Texture1D.MipSlice; break;
		case D3D12_DSV_DIMENSION_TEXTURE1DARRAY: range.mip = dsv.Texture1DArray.MipSlice; array_range(range, dsv.Texture1DArray); break;
		case D3D12_DSV_DIMENSION_TEXTURE2D: range.mip = dsv.Texture2D.MipSlice; break;
		case D3D12_DSV_DIMENSION_TEXTURE2DARRAY: range.mip = dsv.Texture2DArray.MipSlice; array_range(range, dsv.Texture2DArray); break;
		case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY: array_range(range, dsv.Texture2DMSArray); break;
		}
		if (dsv.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || dsv.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
			range.planes |= 2;
		view->read_only_planes = dsv.Flags & range.planes;
		break;
	}
	}

	if (view->resource->dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
		UINT slices = ShimTextureArraySize(desc);
		range.mips = range.mip < desc.MipLevels ? min(range.mips, desc.MipLevels - range.mip) : 0;
		range.slices = range.slice < slices ? min(range.slices, slices - range.slice) : 0;
	}
}

bool ShimViewsOverlap(const ShimView *a, const ShimView *b, UINT planes)
{
	return a && b && a->resource == b->resource && (a->range.planes & b->range.planes & planes)
		&& a->range.mips && b->range.mips && a->range.slices && b->range.slices
		&& a->range.mip < b->range.mip + b->range.mips && b->range.mip < a->range.mip + a->range.mips
		&& a->range.slice < b->range.slice + b->range.slices && b->range.slice < a->range.slice + a->range.slices;
}

void ShimRecreateView(ShimView *view)
{
	ID3D12Resource *resource = view->resource->resource;

	switch (view->kind) {
	case ShimView::SRV:
		dx12_shim->device->CreateShaderResourceView(resource, &view->desc12.srv, view->handle);
		break;
	case ShimView::UAV:
		dx12_shim->device->CreateUnorderedAccessView(resource, NULL, &view->desc12.uav, view->handle);
		break;
	case ShimView::RTV:
		dx12_shim->device->CreateRenderTargetView(resource, &view->desc12.rtv, view->handle);
		break;
	case ShimView::DSV:
		dx12_shim->device->CreateDepthStencilView(resource, &view->desc12.dsv, view->handle);
		break;
	}
}

void ShimDestroyShader(ShimShader *shader)
{
	Dx12ShimForgetPipelines(shader->id);
	delete shader->dxil_form;
}

ShimSamplerState::~ShimSamplerState()
{
	Dx12ShimFreeDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, mHandle);
}

ShimInputLayout::~ShimInputLayout()
{
	Dx12ShimForgetPipelines(mId);
}

void ShimGetResourceDesc(ID3D11Resource *resource, ShimResourceDesc *desc)
{
	*desc = {};
	desc->mips = 1;
	desc->array_size = 1;
	desc->samples = 1;

	switch (ShimResourceOf(resource)->dimension) {
	case D3D11_RESOURCE_DIMENSION_BUFFER: {
		const D3D11_BUFFER_DESC &buffer = static_cast<ShimBuffer*>(resource)->mDesc;

		desc->usage = buffer.Usage;
		desc->bind_flags = buffer.BindFlags;
		desc->cpu_access = buffer.CPUAccessFlags;
		desc->misc_flags = buffer.MiscFlags;
		desc->byte_width = buffer.ByteWidth;
		desc->stride = buffer.StructureByteStride;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
		const D3D11_TEXTURE1D_DESC &tex = static_cast<ShimTexture1D*>(resource)->mDesc;

		desc->usage = tex.Usage;
		desc->bind_flags = tex.BindFlags;
		desc->cpu_access = tex.CPUAccessFlags;
		desc->misc_flags = tex.MiscFlags;
		desc->format = tex.Format;
		desc->mips = tex.MipLevels;
		desc->array_size = tex.ArraySize;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
		const D3D11_TEXTURE2D_DESC &tex = static_cast<ShimTexture2D*>(resource)->mDesc;

		desc->usage = tex.Usage;
		desc->bind_flags = tex.BindFlags;
		desc->cpu_access = tex.CPUAccessFlags;
		desc->misc_flags = tex.MiscFlags;
		desc->format = tex.Format;
		desc->mips = tex.MipLevels;
		desc->array_size = tex.ArraySize;
		desc->samples = tex.SampleDesc.Count;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
		const D3D11_TEXTURE3D_DESC &tex = static_cast<ShimTexture3D*>(resource)->mDesc;

		desc->usage = tex.Usage;
		desc->bind_flags = tex.BindFlags;
		desc->cpu_access = tex.CPUAccessFlags;
		desc->misc_flags = tex.MiscFlags;
		desc->format = tex.Format;
		desc->mips = tex.MipLevels;
		break;
	}
	}
}

ShimTexture2D* ShimWrapTexture2D(ID3D12Resource *resource, D3D12_RESOURCE_STATES state)
{
	ShimTexture2D *texture = new ShimTexture2D();
	D3D11_TEXTURE2D_DESC *desc = &texture->mDesc;

	texture->r.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
	texture->r.resource = resource;
	texture->r.desc = resource->GetDesc();
	texture->r.state = state;
	texture->r.foreign = true;

	*desc = {};
	desc->Width = (UINT)texture->r.desc.Width;
	desc->Height = texture->r.desc.Height;
	desc->MipLevels = texture->r.desc.MipLevels;
	desc->ArraySize = texture->r.desc.DepthOrArraySize;
	desc->Format = texture->r.desc.Format;
	desc->SampleDesc = texture->r.desc.SampleDesc;
	desc->Usage = D3D11_USAGE_DEFAULT;
	if (!(texture->r.desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
		desc->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
	if (texture->r.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
		desc->BindFlags |= D3D11_BIND_RENDER_TARGET;
	if (texture->r.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
		desc->BindFlags |= D3D11_BIND_DEPTH_STENCIL;
	if (texture->r.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		desc->BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

	return texture;
}

bool Dx12ShimIsGameConstants(ID3D11Resource *resource)
{
	ShimResource *r = ShimResourceOf(resource);

	return r->foreign && r->dimension == D3D11_RESOURCE_DIMENSION_BUFFER
		&& (static_cast<ShimBuffer*>(resource)->mDesc.BindFlags & D3D11_BIND_CONSTANT_BUFFER);
}

UINT64 Dx12ShimBufferAddress(ID3D11Buffer *buffer, D3D12_RESOURCE_STATES state, UINT64 offset, UINT64 size)
{
	ShimResource *resource = ShimResourceOf(buffer);
	BYTE *upload;
	UINT64 va;

	if (resource->foreign)
		return resource->foreign_va ? resource->foreign_va + offset : 0;

	if (!resource->cpu_only) {
		if (!Dx12ShimTransition(resource, state))
			return 0;
		return resource->resource->GetGPUVirtualAddress() + offset;
	}

	if (offset >= resource->shadow.size() || !size)
		return 0;
	size = min(size, resource->shadow.size() - offset);
	UINT64 first = offset, end = offset + size;
	if (resource->upload_epoch == dx12_shim_epoch && resource->upload_version == resource->version
	    && !((max(offset, resource->upload_offset) - min(offset, resource->upload_offset)) & 255)) {
		if (offset >= resource->upload_offset && end <= resource->upload_offset + resource->upload_size)
			return resource->upload_va + offset - resource->upload_offset;
		// Ranges bound together are uploaded together, or each draw would
		// upload them again in turn
		first = min(first, resource->upload_offset);
		end = max(end, resource->upload_offset + resource->upload_size);
	}

	// 256 is what a constant buffer view has to be aligned to
	UINT64 padded = (end - first + 255) & ~255ull;
	upload = Dx12ShimUpload(padded, 256, NULL, NULL, &va);
	if (!upload)
		return 0;
	memcpy(upload, resource->shadow.data() + first, (size_t)(end - first));
	memset(upload + (end - first), 0, (size_t)(padded - (end - first)));
	resource->upload_va = va;
	resource->upload_epoch = dx12_shim_epoch;
	resource->upload_version = resource->version;
	resource->upload_offset = first;
	resource->upload_size = end - first;
	return va + offset - first;
}
