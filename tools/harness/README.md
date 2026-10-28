# Test harnesses

Small programs that load `d3d11.dll` the way a game would and check what it
does, without a game. Each folder builds its own program, makes a run folder
with the DLL to test, and has a check script that exits with 1 when a check
fails.

The harness windows are hidden by default. Set `XH_VISIBLE=1` to show them,
for example `XH_VISIBLE=1 bash tools/harness/dx12/check.sh`. Rendering, GPU
copies and presents still run with a hidden window. The `skin` stall
diagnostic shows its window because it checks foreground-window behavior.
Rebuild each harness and copy the new executable into existing run folders
to apply this behavior there; the DLL does not need rebuilding.

| Folder | What it checks |
|---|---|
| `dx12` | The D3D12 shim: matching by hash, hashes carried along GPU copies, the picture against an older DLL, frame time |
| `vk` | The Vulkan path: the test inis of `vk/case/tests` against an older DLL, `[Present]` on frame generation presents |
| `skin` | D3D12 GPU skinning with character mods (`make_mod.py` modes), queue and device lifetimes with D3D11On12 |
| `sampler` | `type = SamplerState` resources and `ps-s0` bindings on D3D11 |
| `cpu` | The CPU work of the DLL on the configuration of an installed importer: call-for-call equality and frame time against a reference DLL |
| `cpu/dx11/desktop` | The Nahida Desktop channel: the codec against the shared vectors, the pipe server, and picking on D3D11 |

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
  `harness.cpp`. The script captures every watched frame in one run per
  pass setting (two processes), without replaying the earlier frames for
  each capture. `capture=N,...` reports zero-based frames without BMP files.
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

`check.sh` runs at least `CHECK_FRAMES` frames (default 60) and at least
`CHECK_SECONDS` seconds (default 0.5) per process. The frame minimum covers
the frame-generation checks after frame 20; the time minimum allows
background shader work to finish. Increase `CHECK_SECONDS` on slower
machines, for example `CHECK_SECONDS=2 bash check.sh`. The harness accepts
`--frames N` to replace its normal duration limit and `--min-seconds N`
to require a minimum duration as well. Timing and profiling runs retain
their existing duration defaults.

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
- `skin.exe fault_at=N` in a run folder made with
  `Rendering.dx12_debug_layer=0 Rendering.dx12_dred=1`: a page fault at frame
  N. The log has the `dx12_dred` report. The debug layer stops the program
  before the fault, so it has to be off.
- `skin.exe stall_at=N` with `Rendering.dx12_stall_dump_seconds=2`: presents
  stop for 4 s, and a `3DM-<time>.dmp` is written in the run folder.

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

### cpu/dx11/desktop

Run `cpu/dx11/build.bat`, then `bash cpu/dx11/desktop/check.sh`. It checks
the codec of `DirectX11/desktop/Protocol.h` against
`testdata/desktop-channel/v1`, then runs `pick.exe` in `cpu/dx11/bin/pick-run`
with `[Desktop] channel = 1`. `pick.exe` draws a scene with a hidden mesh, a
vertex shader that writes `SV_Position` to `o3`, a mod with `handling = skip`
and two `drawindexed` parts, and a cached deferred command list. A client thread checks
the handshake, INI variables, refusals and the PickResults. After every draw
it checks that the pick left the game's state and picture unchanged. It also checks
that overlapping occlusion and pipeline statistics queries retain their results
and that a timestamp disjoint query spanning the scene allows picking.

The script also runs `pick.exe <run folder> uav`, `region` and `devices` with
WARP. These check that UAV slots above 7 are restored, that vertex and index
buffer region hashes retain the data used by a draw when the buffers are
updated afterward, and that picking works on a second device after the first
device has completed a pick. The `region` run enables
`[Rendering] track_region_hashes = 1`.

The `cbzero` run checks that picking preserves a zero-length `b13` constant
buffer range and the rendered image. The `indirect` run checks hits, draw types
and command origins for both `drawinstancedindirect` and
`drawindexedinstancedindirect` in a mod. Both runs use WARP.

The `shader-uav` run uses WARP with vertex shaders that read and write `u12`,
with reflection data stripped. It checks that picking skips these draws and
preserves the UAV data, vertex invocation counts, bindings and image. The
`limits` run checks variable responses at exactly 4 MiB and one byte over the
limit, error request IDs, connection reuse and that a rejected SetVars changes
no variables. Both runs use WARP.

The `coplanar` run checks that a later mesh rejected by `LESS` at the same depth
does not receive a depth-tested hit. The `stencil` run also enables stencil
comparisons and writes to check that probing preserves the rendered image.
The `reconnect` run disconnects during large variable requests and checks that
their responses cannot reach the next connection. These runs use WARP.

The `multiswap` run presents a secondary swap chain after every scene frame,
first on the same device and then on another device. It checks that picking
uses the primary swap chain's frame and window, and that releasing the primary
allows the next scene to drive the channel. The `predication` run leaves a
false predicate bound during Present and checks that picking still returns the
scene's hits, restores the predicate and skips predicated draws. Both runs use WARP.

The `backpressure` run uses WARP and sends 512 GetVars and SetVars requests
interleaved with unknown frames to keep pipe reads ready. It checks that each
request receives one matching reply or `BUSY`, rejected SetVars requests change
no variables, and requests succeed again after the reply queue drains.

`bin/client.exe [pid]` is a console client for checking a game by hand. It
connects to the game's pipe, prints every frame as JSON to the console and
`bin/client.log`, and sends a pick when Enter is pressed. `pick.exe <run folder> idle
<seconds>` draws the scene without its own client, so that `client.exe` can connect.

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
