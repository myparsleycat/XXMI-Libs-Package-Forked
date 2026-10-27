float4 vs(float3 p : POSITION) : SV_Position { return float4(p, 1); }
float4 ps() : SV_Target0 { return float4(0.9, 0.6, 0.2, 1); }
