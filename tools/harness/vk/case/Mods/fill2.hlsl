StructuredBuffer<float4> game_bones : register(t0);
StructuredBuffer<uint4> dumped : register(t1);
RWBuffer<float4> config : register(u0);
RWStructuredBuffer<float4> bones : register(u1);

[numthreads(1, 1, 1)]
void main()
{
	uint i;

	for (i = 0; i < 6; i++)
		config[i] = asfloat(dumped[i]);
	config[6] = asfloat(dumped[6]) * 0.5;
	config[7] = 0;

	bones[0] = 0;
	bones[1] = float4(1, 0, 0, 1);
	bones[2] = game_bones[0];
	bones[3] = 0;
}
