// What the views of the ->Region() copies in CustomShaderRegion show
Buffer<float> region : register(t1);
Buffer<float> clamped : register(t2);
Buffer<float> ref_start : register(t3);
Buffer<float> ref_middle : register(t4);
cbuffer cb_region : register(b0)
{
	float4 cb_first;
};
RWBuffer<float4> output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
	uint n;

	region.GetDimensions(n);
	output[1] = float4(region[0], region[1], region[2], region[3]);
	output[2] = float4(n, 0, 0, 0);
	clamped.GetDimensions(n);
	output[3] = float4(clamped[0], clamped[1], clamped[2], clamped[3]);
	output[4] = float4(n, 0, 0, 0);
	ref_start.GetDimensions(n);
	output[5] = float4(n, ref_start[0], ref_start[3], 0);
	ref_middle.GetDimensions(n);
	output[6] = float4(n, ref_middle[0], ref_middle[1], 0);
	output[7] = cb_first;
}
