// x150 is the frame count of [Present], x151 what its post set the frame before
Texture1D<float4> IniParams : register(t120);
RWTexture2D<float4> tint : register(u0);

[numthreads(4, 4, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	float frames = IniParams.Load(int2(150, 0)).x;
	float shown = IniParams.Load(int2(151, 0)).x;

	tint[id.xy] = float4(frac(frames * 0.0137), frac(shown * 0.0291), frac(frames * 0.071), 1);
}
