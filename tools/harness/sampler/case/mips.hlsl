// The levels the DLL gave mip_checker.dds: their count, the width of level 0,
// texel (0, 0) of level 3 (0 if there is no such level) and of level 0
Texture2D<float4> tex : register(t0);
RWBuffer<float4> output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
	uint width, height, levels;

	tex.GetDimensions(0, width, height, levels);
	output[11] = float4(levels, width, tex.Load(int3(0, 0, 3)).x, tex.Load(int3(0, 0, 0)).x);
}
