# Shader callbacks and draw scope

A `[ShaderOverride]` command list runs at the corresponding stage of the
intercepted application draw. It is not a callback for every draw submitted
by other command lists, and is not a persistent shader-bytecode replacement.

For a graphics draw, the pre-command lists run in VS, HS, DS, GS, PS order
when those stages have overrides. A VS callback may run `checktextureoverride`
and submit replacement geometry **before** the PS callback runs. A
`CustomShader` invoked by that later PS callback cannot change pixels from
those already-submitted replacement draws.

`handling = skip` skips the intercepted application operation; it does not
undo child draws that command lists have already submitted. Likewise,
`draw = from_caller` uses the active application's draw parameters, including
index count and first index. It does not replay all earlier replacement
submeshes. If a mesh override changed the bound index buffer, those parent
parameters may describe a different or degenerate range of that buffer.

To apply a custom shader to a replacement mesh, run that mesh's existing
draw command **inside** the custom-shader scope, with its actual parameters:

```ini
[TextureOverrideMesh]
hash = <the mesh hash>
handling = skip
ib = ResourceReplacementIndices
run = CustomShaderReplacementMesh

[CustomShaderReplacementMesh]
ps = replacement.hlsl
drawindexed = <count>, <first index>, <base vertex>
```

The placeholders above must be replaced with the captured mesh hash and the
replacement mesh's real draw range. They are not a ready-to-load configuration.
`run = CommandListReplacementMesh` can instead execute several existing
submesh draws inside the scope. `CustomShader` restores the shader it changed
when it returns. It does not implicitly re-run all other override callbacks.

Do not change global callback ordering or append a parent `from_caller` draw
to simulate an inherited shader replacement: existing mods depend on the
ordering, scoped shader restoration, hidden geometry, and child draw counts.
An API for opt-in inherited shader replacement would need its own explicit
contract, including custom-shader precedence and pipeline-state compatibility.

The [shader-scope harness](../../../tools/harness/shader-scope/README.md)
demonstrates the distinction on both D3D11 and D3D12 with synthetic geometry.
