Texture2DArray<uint> image : register(t0);
ByteAddressBuffer rawInput : register(t1);
StructuredBuffer<uint2> structuredInput : register(t2);

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
