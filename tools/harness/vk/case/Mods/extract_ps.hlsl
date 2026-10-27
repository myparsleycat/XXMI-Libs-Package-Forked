struct V2P {
    float4 pos : SV_Position;
    nointerpolation uint id : TEXCOORD0;
    nointerpolation uint4 raw_bits : TEXCOORD1;
};

RWStructuredBuffer<uint4> Dumped : register(u7);

float4 main(V2P i) : SV_Target {
    Dumped[i.id] = i.raw_bits;
    return float4(0, 0, 0, 0);
}
