// What the game's vertex shader has for its constants, as they are in memory
cbuffer Game : register(b0) { float4 rows[4]; float4 tint[4]; };
StructuredBuffer<float4> game_bones : register(t0);
RWBuffer<float4> config : register(u0);
RWStructuredBuffer<float4> bones : register(u1);

[numthreads(1, 1, 1)]
void main()
{
	uint i;

	for (i = 0; i < 4; i++)
		config[i] = rows[i];
	config[4] = tint[0];
	config[5] = tint[1];
	// No colour of the vertex: all there is is the bone
	config[6] = 0;
	config[7] = 0;

	bones[0] = 0;
	// What a vertex of the mod reads if its index is read like one of the game's
	bones[1] = float4(1, 0, 0, 1);
	bones[2] = game_bones[0];
	bones[3] = 0;
}
