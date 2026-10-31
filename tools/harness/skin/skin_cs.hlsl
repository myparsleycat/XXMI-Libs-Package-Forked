struct Vertex { float3 p; float3 n; float4 t; };
struct Weight { float4 w; uint4 i; };
StructuredBuffer<Vertex> source : register(t0);
#ifdef RIGID
StructuredBuffer<uint> weights : register(t1);
#else
StructuredBuffer<Weight> weights : register(t1);
#endif
StructuredBuffer<float4x4> bones : register(t2);
RWStructuredBuffer<Vertex> output : register(u0);
cbuffer Count : register(b0) { uint count; };

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= count)
		return;
	Vertex v = source[id.x];
#ifdef RIGID
	v.p += bones[weights[id.x]][3].xyz;
#else
	Weight w = weights[id.x];
	v.p += bones[w.i.x][3].xyz * w.w.x;
#endif
	output[id.x] = v;
}
