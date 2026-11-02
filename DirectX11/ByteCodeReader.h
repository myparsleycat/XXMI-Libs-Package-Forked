#pragma once

#include "ShaderRegex.h"

bool get_shader_bindings_from_bytecode(const void* data, size_t size, ShaderBindings* out_bindings);
// One bit for each t slot declared as a structured or raw buffer.
void get_buffer_srv_slots(const ShaderBindings& bindings, uint64_t slots[2]);
// For a shader not made from the game's bytecode. A driver can give the handle of
// a released replacement to a new shader, which forget_replacement_srv_slots() is for.
void note_replacement_srv_slots(ID3D11DeviceChild* shader, const void* bytecode, size_t size);
void forget_replacement_srv_slots(ID3D11DeviceChild* shader);
