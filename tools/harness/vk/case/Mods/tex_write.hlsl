// A texture a mod writes every frame, for the pixel shader of the game
RWTexture2D<float4> tex : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	tex[id.xy] = float4(id.x / 63.0, id.y / 63.0, ((id.x ^ id.y) & 8) ? 1.0 : 0.25, 1);
}
