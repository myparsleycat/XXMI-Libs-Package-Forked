#version 450
// The vertex shader of a character as the game the EFMI mods are for has it:
// constants of the frame, the constants of 256 instances in one block of
// 65536 bytes, the bones of everything in a storage buffer, and the vertices
// in streams
layout(set = 0, binding = 0, std140) uniform UnityPerFrame { mat4 unity_MatrixVP; vec4 frame_params[4]; };
layout(set = 2, binding = 0, std140) uniform UnityInstancing { vec4 instance_rows[4096]; };
layout(set = 0, binding = 2, std430) readonly buffer Bones { vec4 bones[]; };
layout(location = 0) in vec3 in_POSITION0;
layout(location = 1) in vec4 in_TEXCOORD0;
layout(location = 2) in uvec4 in_BLENDINDICES0;
layout(location = 3) in vec4 in_BLENDWEIGHT0;
layout(location = 4) in vec3 in_POSITION1;
layout(location = 5) in vec4 in_COLOR0;
layout(location = 0) out vec4 vs_TEXCOORD0;
void main()
{
	// In the order the blocks are to be numbered in: that of their first use
	vec4 frame = frame_params[1] * 0.0 + unity_MatrixVP[0] * 0.0;
	int row = gl_InstanceIndex * 16;
	vec4 placed = instance_rows[row + 3];
	uint first_bone = uint(instance_rows[row + 4].x);
	vec4 bone = bones[in_BLENDINDICES0.x * 3 + first_bone];
	vec3 position = in_POSITION0 * 0.01 + in_POSITION1 * 0.0 + placed.xyz * 0.001 + bone.xyz * 0.0;
	gl_Position = vec4(position.xy, 0.5, 1.0) + frame;
	vs_TEXCOORD0 = in_TEXCOORD0 + in_BLENDWEIGHT0 * 0.0 + in_COLOR0 * 0.0;
}
