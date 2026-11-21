// What the [Filter] lines of [Present] left: the IniParams they set and
// texel (1, 0) of the views fetched from ps-t2 and ps-t3
Texture1D<float4> IniParams : register(t120);
Texture2D<float4> after2 : register(t5);
Texture2D<float4> after3 : register(t6);
Texture2D<float4> restored3 : register(t7);
RWBuffer<float4> output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
	output[8] = IniParams.Load(int2(0, 0));
	output[9] = IniParams.Load(int2(1, 0));
	output[10] = float4(after2.Load(int3(1, 0, 0)).x, after3.Load(int3(1, 0, 0)).x, restored3.Load(int3(1, 0, 0)).x, 0);
}
