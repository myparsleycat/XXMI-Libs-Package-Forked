#if defined(DXIL_RAY_QUERY)
RaytracingAccelerationStructure scene : register(t0);
Texture2D<uint> image : register(t1);
RWBuffer<uint> result : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    RayDesc ray = {float3(0, 0, 0), 0, float3(0, 0, 1), 1};
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {}
    result[0] = image.Load(int3(0, 0, 0)) + (query.CommittedStatus() == COMMITTED_NOTHING ? 37 : 1);
}
#elif defined(DXIL_SRV_OVERFLOW) || defined(DXIL_SRV_UNBOUNDED)
#ifdef DXIL_SRV_OVERFLOW
Texture2D<uint> images[2] : register(t127);
#else
Texture2D<uint> images[] : register(t127);
#endif
RWBuffer<uint> result : register(u0);

[numthreads(2, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    result[thread.x] = images[NonUniformResourceIndex(thread.x)].Load(int3(0, 0, 0));
}
#else
ByteAddressBuffer rawInput : register(t0);
StructuredBuffer<uint2> structuredInput : register(t1);
Texture2DArray<uint> image : register(t120);
#ifdef DXIL_INDEXED
Texture2DArray<uint> images[2] : register(t63);
#endif

RWTexture2DArray<uint> destination : register(u0);
RWBuffer<uint> result : register(u1);
RWByteAddressBuffer rawOutput : register(u2);
RWStructuredBuffer<uint2> structuredOutput : register(u3);

[numthreads(1, 1, 1)]
void main()
{
    uint value = image.Load(int4(0, 0, 0, 0)) + rawInput.Load(0) + structuredInput[0].y;
    destination[uint3(0, 0, 0)] = value;
    result[0] = value;
    rawOutput.Store(0, value);
    structuredOutput[0] = uint2(value, value);
}

#ifdef DXIL_INDEXED
[numthreads(2, 1, 1)]
void indexed(uint3 thread : SV_DispatchThreadID)
{
    result[thread.x] = images[NonUniformResourceIndex(thread.x)].Load(int4(0, 0, 0, 0));
}
#endif
#endif
