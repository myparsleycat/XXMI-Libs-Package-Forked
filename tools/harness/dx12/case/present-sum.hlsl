// Adds to what the texture held, every time it is executed
RWTexture2D<float> sum : register(u0);

[numthreads(4, 4, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	sum[id.xy] = frac(sum[id.xy] + 0.0371);
}
