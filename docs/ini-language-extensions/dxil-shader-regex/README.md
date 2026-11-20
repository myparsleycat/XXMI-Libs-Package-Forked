# DXIL Shader Regex

`DxilPattern` sections match or rewrite DXC's disassembly of a Direct3D 12 game's shader. They use the same PCRE2 syntax and replacement rules as `Pattern`; legacy `Pattern` sections continue to target Direct3D 11 shaders.

```ini
[ShaderRegexGlow]
shader_model = ps_5_0
filter_index = 1234.1

[ShaderRegexGlow.InsertDeclarations]
dcl_resource_texture2d (float,float,float,float) t60

[ShaderRegexGlow.DxilPatternA]
<PCRE2 expression matching the shader's DXC disassembly>

[ShaderRegexGlow.DxilPatternA.Replace]
<replacement DXC disassembly>
```

* `shader_model` selects a graphics shader stage. Compute (`cs_*`) DXIL groups are rejected. For a DXIL group that also has legacy `Pattern` sections, use `dx12_match_registers`, `dx12_match_render_targets`, or `dx12_match_cbN_size` to select the matching D3D12 shader; the D3D11 pattern is not applied to DXIL.
* The lines of a pattern or `.Replace` section are joined without a separator, after leading and trailing whitespace is removed. A replacement that spans several lines needs `\n` at the end of each line. Lines starting with `;` are ini comments and lines starting with `[` are section headers, so a pattern line cannot start with either; capture the text before it instead, for example `(?<pre>!\{)\[16 x i32\]` replaced by `${pre}[16 x i32]`. A line whose first `=` has nothing after it is dropped with a "No value found" warning, so keep an `=` and the text that follows it on the same line, for example `%1 = call ...` rather than `%1 =` followed by `call ...` on the next line.
* The patcher does not update `!dx.viewIdState`. A replacement that makes an output depend on another input should update that metadata itself.
* Each `DxilPattern*` is applied in name order. Without `.Replace`, it only has to match. With `.Replace`, the first match is replaced. If any pattern fails, that group's changes are rolled back.
* `InsertDeclarations` can add `dcl_resource_texture1d (float,float,float,float) tN` or `dcl_resource_texture2d (float,float,float,float) tN` for `t0`–`t127`. The patcher adds the corresponding DXIL resource metadata and handle in space 0. A replacement can refer to the handle as `%zz_d3dmigoto_tN`; `%zz_*` values introduced by a replacement are renumbered before assembly.
* A patch that adds SRVs other than IniParams is used only for draws where a mod binds at least one of those SRVs; added SRVs the mod leaves unbound read zeros. Elsewhere the game's original shader is used, so such a patch must produce the original result when its added SRVs are zero. A patch that adds no SRV, or only IniParams, is used for every draw of the shader.
* `dcl_sampler sN, mode_default` adds a sampler in space 0, accessible as `%zz_d3dmigoto_sN`. Samplers absent from the game's root signature can use the default state or a `<stage>-sN` binding; see [Sampler States](../resources/sampler-states.md#direct3d-12-and-vulkan).
* Writable UAV execution is not enabled. `dcl_uav_raw uN` patches and rewrites of shaders that already declare a UAV are rejected, including UAVs outside register space 0. The assembled result is reflected again, so a replacement cannot inject a UAV by editing DXIL metadata directly. Shaders using directly indexed descriptor heaps, and shaders whose resources cannot be reflected, are also left unmodified.
* DXIL groups may not contain `SpirvPattern` sections. A group with legacy `Pattern` sections must use the D3D11 `dcl_resource_texture1d (...) tN` or `dcl_resource_texture2d (...) tN` declaration syntax.

The shader is used unmodified if it does not match, or if the bundled `dxcompiler.dll` / `dxil.dll` rejects the patched disassembly. Patterns target the DXC version shipped with the runtime; DXBC patterns are never translated automatically. Added resources require the original shader to have a resource metadata list; other resource types and dynamic buffer layouts are not covered.

Use `dx12_dump_hashes = 1` in `[Hunting]` to list D3D12 shader hashes and declarations in `dx12_shaders.csv`. `dx12_match_*` keys can identify a shader before a `DxilPattern` is applied.
