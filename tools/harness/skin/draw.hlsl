#ifdef BINDINGS
cbuffer Place : register(b0) { float4 place[48]; };
float4 vs(float3 p : POSITION) : SV_Position { return float4(p + place[0].xyz + place[47].xyz, 1); }
StructuredBuffer<float4> Game : register(t0);
float4 ps() : SV_Target0 { return float4(0.9 + Game[0].x * 0.0001, 0.6, 0.2, 1); }
#else
#ifdef PLACE
cbuffer Place : register(b0) { float4 place; };
float4 vs(float3 p : POSITION) : SV_Position { return float4(p + place.xyz, 1); }
#else
float4 vs(float3 p : POSITION) : SV_Position { return float4(p, 1); }
#endif
float4 ps() : SV_Target0 { return float4(0.9, 0.6, 0.2, 1); }
#endif
