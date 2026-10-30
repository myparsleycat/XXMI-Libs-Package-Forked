cbuffer Game : register(b0) { float4 rows[4]; float4 tint[4]; };
RWBuffer<float4> config : register(u0);
RWStructuredBuffer<float4> bones : register(u1);

[numthreads(1, 1, 1)]
void main()
{
    float4 colour = tint[2];
    if (colour.g > 0.5 && (config[6].r != 1 || config[6].g != 0))
        colour = float4(1, 0, 1, 1);
    for (uint i = 0; i < 4; i++) {
        config[i] = rows[i];
        bones[i] = 0;
    }
    config[4] = tint[0];
    config[5] = tint[1];
    config[6] = colour;
    config[7] = 0;
}
