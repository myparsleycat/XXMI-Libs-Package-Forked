struct V2P {
    float4 pos : SV_Position;
    nointerpolation uint id : TEXCOORD0;
    nointerpolation uint4 raw_bits : TEXCOORD1;
};

cbuffer CB0 : register(b0) { float4 cb_data[8]; };

V2P main(uint id : SV_VertexID) {
    V2P o;
    o.pos = float4(-0.98 + (id % 64) * 0.03, -0.98 + (id / 64) * 0.03, 0.5, 1.0);
    o.id = id;
    o.raw_bits = asuint(cb_data[id]);
    return o;
}
