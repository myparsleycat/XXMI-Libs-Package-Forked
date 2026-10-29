// Included with COLUMN, SAMPLER_SLOT and UV defined
Texture2D<float4> tex : register(t0);
SamplerState smp : register(SAMPLER_SLOT);

#ifdef VERTEX_SHADER
void main(uint id : SV_VertexID, out float4 pos : SV_Position)
{
	float2 uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
#endif

#ifdef PIXEL_SHADER
float4 main(float4 pos : SV_Position) : SV_Target0
{
	if ((uint)pos.x != COLUMN)
		discard;
	return tex.SampleLevel(smp, UV, 0);
}
#endif

#ifdef COMPUTE_SHADER
RWBuffer<float4> output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
	output[COLUMN] = tex.SampleLevel(smp, UV, 0);
}
#endif
