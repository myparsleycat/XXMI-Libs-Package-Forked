#version 450
layout(set = 1, binding = 0, std140) uniform UnityPerMaterial { vec4 _BaseColor; };
layout(set = 1, binding = 2) uniform texture2D _MainTex;
layout(set = 1, binding = 4) uniform sampler sampler_MainTex;
layout(location = 0) in vec4 vs_COLOR0;
layout(location = 0) out vec4 SV_Target0;
void main() { SV_Target0 = vs_COLOR0 * texture(sampler2D(_MainTex, sampler_MainTex), vec2(0.5)); }
