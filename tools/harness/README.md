# Test harnesses

Small programs that load `d3d11.dll` the way a game would and check what it
does, without a game. Each folder builds its own program, makes a run folder
with the DLL to test, and has a check script that exits with 1 when a check
fails.

| Folder | What it checks |
|---|---|
| `dx12` | The D3D12 shim: matching by hash, hashes carried along GPU copies, the picture against an older DLL, frame time |
| `vk` | The Vulkan path: the test inis of `vk/case/tests` against an older DLL, `[Present]` on frame generation presents |
| `skin` | D3D12 GPU skinning with character mods (`make_mod.py` modes), queue and device lifetimes with D3D11On12 |
| `sampler` | `type = SamplerState` resources and `ps-s0` bindings on D3D11 |
| `cpu` | The CPU work of the DLL on the configuration of an installed importer: call-for-call equality and frame time against a reference DLL |

## Requirements

- Visual Studio 2022 with the C++ tools (any edition) and the Windows SDK
  (`dxc.exe` for `skin`)
- The Vulkan SDK with `VULKAN_SDK` set (`glslangValidator` for `vk`)
- Python 3.12 or later as `python`
- Git Bash: run the `.sh` scripts from it, not from WSL
- A GPU with D3D12 and Vulkan support

## Use

Build the DLL first:

```bash
bash tools/harness/common/build_dll.sh
```

After a change to a header that many files include, pass `rebuild`.
Incremental builds can keep stale objects. Never run two builds at once.

Then, for `dx12`, `vk` and `skin`, run these steps in the harness folder:

1. `build.bat` builds the program into `bin/`. Run it from cmd or PowerShell.
2. `bash setup.sh` makes `run/` with `x64/Release/d3d11.dll`.
3. `bash setup.sh ref <older d3d11.dll>` makes `ref/`. Do this for the checks
   that compare with an older DLL.
4. `bash check.sh` runs the checks.

`setup.sh` writes the `d3dx.ini` of the package (`Dependencies/d3dx.ini`) with
the settings that harness needs. To change more settings, append
`Section.key=value` arguments after the folder and the DLL.

### dx12

- `check.sh test`: `case/test.ini` hides the meshes and textures whose
  hashes it lists. The script compares which ones are hidden with a fixed list.
- `check.sh move`: mesh A's vertices move between buffers by GPU copies,
  across lists and queues. The cases are in the comment at `move_test` in
  `harness.cpp`. The script checks frame by frame whether A stays hidden.
- `check.sh lowmip`: a texture is filled by a GPU copy from the second mip
  level of a texture whose hash `case/test.ini` lists. With
  `track_texture_updates = 1` the copy has to be drawn with
  `match_lower_mip_textures = 0` and hidden with `1`.
- `check.sh ref`: compares the pictures of `run` and `ref` with the mod in
  `case/Mods` and with `test.ini`.
- `bench.sh [runs] [args]`: reports the recording time per frame with no DLL,
  with `ref` and with `run`.

`case/Mods/mod/mod.ini` was made by `make_mod.py` from a hash dump of the
harness's own buffers. It is kept fixed so that a DLL that hashes wrongly
fails the checks instead of matching itself. Run `make_mod.py` again only
when `harness.cpp` changes its meshes.

### vk

- `check.sh tests`: draws every `case/tests/test_*.ini` and `bench_*.ini`
  with `run` and with `ref`. The pixels and the picture checksum have to match.
- `check.sh fg`: runs every `fg_*.ini` with frame generation
  (`--generated 1`). The mod's triangle has to be on screen at the end.
- `check.sh lowmip`: draws `case/tests/lowmip_texhash.ini` with `--lowmip`,
  where `vkCmdCopyImage` fills the texture from the second mip level of a
  texture whose hash the ini lists, 2x2 from 4x4 and with `--wide` 2x1 from
  4x1. The texture has to be replaced only with
  `track_texture_updates = 1` and `match_lower_mip_textures = 1`.
- `try.sh <test> [args]`: runs one test ini and prints the summary from
  `vk_report.txt`.

Run the other harness options by hand and read their output. These are
`--overlay`, `--bonetick`, `--unmapped`, `--map2`, `--gpufilled`, `--reload`
and `--appear`. They are described in `harness.cpp`.

### skin

- `check.sh`: runs each mode of `make_mod.py` with its `skin.exe` arguments
  and compares which spots are drawn with a fixed list. It then runs
  `queuelife`, `queuegone` and `devicelife`.
- `try.sh <mode> [frames] [args]`: runs one mode.

Mode 8 (`drawfirst`, with the draws checking only vb0 and vb2) does not run
the mod's `...IB` section. The game's draw gets the mod's skinned vertices
with the game's indices, which cover only the lower rows of the bar. This is
the same as on D3D11.

### sampler

`bash run.sh` makes `run/new` from `case/` and the DLL just built, then checks
the last frame and the warnings for the invalid sections in `d3d11_log.txt`.

To compare with a package DLL from before sampler slots, or with the upstream
DLL, run `run.sh old` or `run.sh stock` with `XXMI_PACKAGES` set to the
launcher's packages folder. The checks are expected to fail with those DLLs.

### cpu

This harness runs the configuration of installed importers through the DLL.
It needs `XXMI_ROOT`, the folder that holds `GIMI`, `SRMI`, `ZZMI`, `WWMI` and
`EFMI` as XXMI deploys them. Run folders go to `XH_RUNS`, which defaults to
`~/.xh`. Keep that path short: the DLL keeps paths in `MAX_PATH` buffers.

1. Run `dx11/build.bat`.
2. Run `bash xh.sh ref` to make the DLL just built the reference.
3. Run `bash xh.sh setup` once.
4. Rebuild the DLL, then run `bash xh.sh check` or `bash xh.sh bench`. They
   copy the DLL of `x64/Release` into the `cur` run folders first.

`xh.sh setup` writes `scenes/<importer>.scene` from the installed mods. These
files name the user's mods, so they are not tracked.

`check` and `diff` run with `shader_regex_background = 2` where the importer
has 1. With 1, a ShaderRegex takes effect when its background job ends, so
the frames differ from run to run. `bench` and `run` use the setting as
installed.

- `dx12.sh` runs the `dx12` harness on the same scenes, for ZZMI and WWMI.
- `vk.sh` runs the `vk` harness on the same scenes, for EFMI.
- `ab.sh` compares the first appearance of mods between variants.

Remove run folders with `clean.py <importer|--all>`, never with `rm -rf`.
`Core`, `Mods` and `ShaderFixes` in them are junctions to the installed
folders.

`common/profile.inc` is the sampling profiler that the `profile` options of
the harnesses use. `dx12/symbolize.py` turns offsets into `d3d11.dll` into
function names and lines.
