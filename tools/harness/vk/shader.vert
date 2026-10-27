#version 450
layout(set = 0, binding = 1, std140) uniform UnityPerDraw { mat4 unity_ObjectToWorld; vec4 tint[4]; };
layout(set = 0, binding = 2, std430) readonly buffer Bones { vec4 bones[]; };
layout(location = 0) in vec3 in_POSITION0;
layout(location = 1) in vec4 in_COLOR0;
layout(location = 2) in uvec4 in_BLENDINDICES0;
layout(location = 0) out vec4 vs_COLOR0;
void main() { gl_Position = vec4(in_POSITION0, 1.0); vs_COLOR0 = in_COLOR0 * tint[2] + bones[1 + (in_BLENDINDICES0.x >> 8)]; }
