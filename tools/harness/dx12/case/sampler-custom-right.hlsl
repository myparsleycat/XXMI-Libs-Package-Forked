Texture2D tex : register(t0);
SamplerState s : register(s15);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    clip(position.x - 16.0);
    return float4(tex.Sample(s, uv).g, 0, 0, 1);
}
