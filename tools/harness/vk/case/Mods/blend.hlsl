struct Vertex { float4 a, b; };
RWStructuredBuffer<Vertex> rw_buffer : register(u5);
StructuredBuffer<Vertex> base : register(t50);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	rw_buffer[id.x] = base[id.x];
}
