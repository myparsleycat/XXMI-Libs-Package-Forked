"""Synthetic D3D11/D3D12 callback-scope checks, without a game or its assets."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def fnv64(data):
    value = 0
    for byte in data:
        value = ((value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF) ^ byte
    return f"{value:016x}"


def text(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")


def execute(exe, arguments, directory):
    result = subprocess.run(
        [str(exe), *map(str, arguments)], cwd=directory,
        capture_output=True, timeout=90,
    )
    (directory / "stdout.log").write_bytes(result.stdout)
    (directory / "stderr.log").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(f"{directory.name}: exit {result.returncode}; see stderr.log")
    return result.stdout


def check_late_draw_order(api, log, notes):
    if api == "dx11":
        draws = [match.group(1) for line in log.splitlines()
                 if (match := re.search(r"3DMigoto\s+\[[^\]]+\] (Draw.*)$", line))]
        expected = ["DrawIndexed(3, 0, 0)",
                    "Draw = from_caller -> DrawIndexedInstanced(3, 1, 3, 0, 0)"]
    else:
        draws = [line.partition(",")[0] for line in notes]
        expected = ["Mod draw: 3 from 0 indexed", "Mod draw: 3 from 3 indexed"]
    if draws != expected:
        raise AssertionError(
            f"{api}/late: expected replacement draw before parent replay; got {draws!r}")
    return draws



CORE_INI = r"""namespace = WWMIv1
[Constants]
global $shapekey_vertex_count = 0
global $shapekey_checksum_batch0 = 0
global $shapekey_vertex_offset_original_batch0 = 0
global $shapekey_checksum_batch1 = 0
global $shapekey_vertex_offset_original_batch1 = 0
global $shapekey_vertex_count_batch0 = 0
global $shapekey_dispatch_size_y_original_batch0 = 0
global $shapekey_vertex_count_batch1 = 0
global $shapekey_dispatch_size_y_original_batch1 = 0
global $game_batch_checksum = -1
global $game_batch_vertex_offset = -1
global $observed_y0 = 0
global $observed_y1 = 0

[ShaderOverrideShapeKeyLoaderCS]
hash = {cs}
allow_duplicate_hash = true
filter_index = 3381.3333
handling = skip
if cs-u0 >= 1
    $game_batch_checksum = cs-cb0->Uint(0) + cs-cb0->Uint(4) + cs-cb0->Uint(8) + cs-cb0->Uint(12)
    $game_batch_vertex_offset = cs-cb0->Uint(65 * 16 + 4)
    checktextureoverride = cs-u0
endif

[CommandListShapeKeyBatchOverrider]
y0 = $shapekey_vertex_offset_original_batch0
y1 = $shapekey_vertex_offset_original_batch1
if $game_batch_vertex_offset >= 0 && $game_batch_checksum == $shapekey_checksum_batch0
    y0 = $game_batch_vertex_offset
elif $game_batch_vertex_offset >= 0 && $game_batch_checksum == $shapekey_checksum_batch1
    y1 = $game_batch_vertex_offset
endif
$observed_y0 = y0
$observed_y1 = y1

[CommandListLoadShapeKeysBatch]
if $game_batch_vertex_offset >= 0 && $game_batch_checksum == $shapekey_checksum_batch0
    $shapekey_vertex_count = $shapekey_vertex_count_batch0
elif $game_batch_vertex_offset >= 0 && $game_batch_checksum == $shapekey_checksum_batch1
    $shapekey_vertex_count = $shapekey_vertex_count_batch1
elif THREAD_GROUP_COUNT_Y == $shapekey_dispatch_size_y_original_batch0
    $shapekey_vertex_count = $shapekey_vertex_count_batch0
elif THREAD_GROUP_COUNT_Y == $shapekey_dispatch_size_y_original_batch1
    $shapekey_vertex_count = $shapekey_vertex_count_batch1
else
    if $shapekey_dispatch_size_y_original_batch0 > $shapekey_dispatch_size_y_original_batch1
        $shapekey_vertex_count = $shapekey_dispatch_size_y_original_batch0
    else
        $shapekey_vertex_count = $shapekey_dispatch_size_y_original_batch1
    endif
endif
"""

# Dispatch X: batches as (checksum, vertex offset, dispatch Y, vertex count),
# then (y0, y1, vertex count) with ->Uint and with a DLL that drops those lines.
# The game's cb0 sums to 54 with vertex offset 700 + X, except X=3 (sum 89)
# and X=5 (not in upload memory); every dispatch has Y = 24.
SHAPE_KEY_FIXTURES = [
    ([(12, 10, 24, 100), (54, 20, 40, 101)], (10, 701, 101), (10, 20, 100)),
    ([(54, 30, 25, 200), (0, 0, 0, 201)], (702, 0, 200), (30, 0, 25)),
    ([(100, 31, 24, 300), (101, 32, 40, 301)], (31, 32, 300), (31, 32, 300)),
    ([(54, 41, 24, 400), (54, 42, 40, 401)], (704, 42, 400), (41, 42, 400)),
    ([(54, 51, 24, 500), (55, 52, 40, 501)], (51, 52, 500), (51, 52, 500)),
]


def prepare_cb_case(exe, dll, dependencies, directory):
    directory.mkdir()
    shutil.copy2(dll, directory / "d3d11.dll")
    for name in ("dxcompiler.dll", "dxil.dll"):
        shutil.copy2(dependencies / name, directory / name)
    for font in dependencies.glob("*.spritefont"):
        shutil.copy2(font, directory / font.name)
    text(directory / "d3dx.ini", f"""[Loader]
target = {exe.name}
[Logging]
log_level = info
[System]
skip_early_includes_load = false
[Include]
include_recursive = Mods
[Rendering]
extended_rendering = true
shader_hash = 3dmigoto
[Hunting]
hunting = 0
""")


def check_cb_log(directory):
    log = (directory / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace")
    if "Unrecognised entry:" in log or "Error compiling" in log:
        raise AssertionError(f"{directory.name}: invalid configuration or shader")


def check_constant_reads_dx11(exe, dll, dependencies, output, shaders, vs):
    directory = output / "dx11_cb"
    prepare_cb_case(exe, dll, dependencies, directory)
    text(directory / "Mods" / "probe.ini", f"""[Constants]
global $uint_read = -9
global $float_read = -9

[ShaderOverrideVS]
hash = {vs}
$uint_read = vs-cb0->Uint(0)
$float_read = vs-cb0->Float(0)
if $uint_read != -1 || $float_read != -1
    handling = skip
endif
""")
    actual = json.loads(execute(exe, ["dx11", directory / "d3d11.dll", shaders], directory))
    expected = dict(red=256, green=0, black=0, other=0)
    if actual != expected:
        raise AssertionError(f"dx11/cb: expected red (->Uint and ->Float return -1); got {actual}")
    check_cb_log(directory)
    print(f"dx11/cb: PASS {actual}", flush=True)
    return dict(api="dx11", mode="cb", pixels=actual, passed=True)


def check_constant_reads(exe, dll, dependencies, output, shaders, vs):
    directory = output / "dx12_cb"
    prepare_cb_case(exe, dll, dependencies, directory)

    text(shaders / "cb_cs.hlsl", """cbuffer GameBatch : register(b0) { uint4 batch[66]; }
RWBuffer<uint> offsets : register(u0);
[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x == 0 && id.y == 0) offsets[0] = batch[0].x + batch[1].x + batch[65].y;
}
""")
    execute(exe, ["compile-shim", shaders / "cb_cs.hlsl", shaders / "cb_cs.bin"], shaders)
    cs = fnv64((shaders / "cb_cs.bin").read_bytes())
    text(directory / "Mods" / "core.ini", CORE_INI.replace("{cs}", cs))

    count = len(SHAPE_KEY_FIXTURES)
    ini = f"""[Constants]
global $new = 0
global $old = 0
global $float_read = -9
global $past_end = -9
global $bound_read = -9
global $unbound_read = -9

[ResourceOwnConstants]
type = Buffer
format = R32_UINT
data = 7 7 7 7

; Reads that do not depend on the shape key logic
[ShaderOverrideReads]
hash = {cs}
allow_duplicate_hash = true
if THREAD_GROUP_COUNT_X == 1
    $float_read = cs-cb0->Float(16)
    $past_end = cs-cb0->Uint(66 * 16)
elif THREAD_GROUP_COUNT_X == 2
    cs-cb0 = ResourceOwnConstants
    $bound_read = cs-cb0->Uint(0)
    cs-cb0 = null
    $unbound_read = cs-cb0->Uint(0)
endif

[TextureOverrideShapeKeys]
match_type = Buffer
match_byte_width = 64
"""
    for x, (batches, new, old) in enumerate(SHAPE_KEY_FIXTURES, 1):
        ini += f"{'if' if x == 1 else 'elif'} THREAD_GROUP_COUNT_X == {x}\n"
        for i, (checksum, offset, y, vertex_count) in enumerate(batches):
            ini += rf"""    $\WWMIv1\shapekey_checksum_batch{i} = {checksum}
    $\WWMIv1\shapekey_vertex_offset_original_batch{i} = {offset}
    $\WWMIv1\shapekey_dispatch_size_y_original_batch{i} = {y}
    $\WWMIv1\shapekey_vertex_count_batch{i} = {vertex_count}
"""
        ini += r"""    run = CommandList\WWMIv1\ShapeKeyBatchOverrider
    run = CommandList\WWMIv1\LoadShapeKeysBatch
"""
        for (y0, y1, vertex_count), counter in [(new, "$new"), (old, "$old")]:
            condition = (rf"$\WWMIv1\observed_y0 == {y0} && $\WWMIv1\observed_y1 == {y1}"
                         rf" && $\WWMIv1\shapekey_vertex_count == {vertex_count}")
            if counter == "$new" and x == 5:
                condition += r" && $\WWMIv1\game_batch_vertex_offset == -1"
            ini += f"""    if {condition}
        {counter} = {counter} + 1
    endif
"""
    ini += f"""endif

[ShaderOverrideVS]
hash = {vs}
if $new == {count} && $float_read == 2.5 && $past_end == -1 && $bound_read == -1 && $unbound_read == 10
elif $old == {count}
    handling = skip
    run = CustomShaderOld
else
    handling = skip
endif

[CustomShaderOld]
ps = green.hlsl
draw = from_caller
"""
    text(directory / "Mods" / "probe.ini", ini)
    text(directory / "Mods" / "green.hlsl", "float4 main():SV_Target { return float4(0,1,0,1); }\n")
    actual = json.loads(execute(exe, ["dx12-cb", directory / "d3d11.dll", shaders], directory))
    expected = dict(red=256, green=0, black=0, other=0)
    if actual != expected:
        raise AssertionError(
            f"dx12/cb: expected red (->Uint/->Float read as specified); green means the DLL "
            f"dropped the ->Uint lines and every batch fell back; got {actual}")
    check_cb_log(directory)
    print(f"dx12/cb: PASS {actual}", flush=True)
    return dict(api="dx12", mode="cb", pixels=actual, passed=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True,
                        help="Directory containing dxcompiler.dll and dxil.dll")
    parser.add_argument("--output", type=Path, required=True,
                        help="New, empty output directory (never overwritten)")
    parser.add_argument("--cb-only", action="store_true",
                        help="Run only the constant buffer read cases")
    options = parser.parse_args()
    exe, dll, dependencies, output = (
        p.resolve() for p in (options.exe, options.dll, options.dependencies, options.output)
    )
    for path in (exe, dll, dependencies / "dxcompiler.dll", dependencies / "dxil.dll"):
        if not path.is_file():
            parser.error(f"missing file: {path}")
    output.mkdir(parents=True, exist_ok=False)
    shaders = output / "shaders"
    shaders.mkdir()
    execute(exe, ["compile", shaders], shaders)
    vs = fnv64((shaders / "vs.bin").read_bytes())
    ps = fnv64((shaders / "ps.bin").read_bytes())
    cases = []
    for api in (() if options.cb_only else ("dx11", "dx12")):
        for mode in ("native", "plain", "mesh", "late", "scoped", "scoped_off"):
            directory = output / f"{api}_{mode}"
            directory.mkdir()
            if mode != "native":
                shutil.copy2(dll, directory / "d3d11.dll")
                for name in ("dxcompiler.dll", "dxil.dll"):
                    shutil.copy2(dependencies / name, directory / name)
                for font in dependencies.glob("*.spritefont"):
                    shutil.copy2(font, directory / font.name)
                text(directory / "d3dx.ini", f"""[Loader]
target = {exe.name}
require_admin = false
[Logging]
log_level = {'debug' if api == 'dx11' and mode == 'late' else 'info'}
[System]
skip_early_includes_load = false
[Include]
include_recursive = Mods
[Rendering]
extended_rendering = true
shader_hash = 3dmigoto
[Hunting]
hunting = 0
dx12_dump_hashes = true
""")
                if mode != "plain":
                    mod = directory / "Mods" / "Mesh"
                    mod.mkdir(parents=True)
                    (mod / "mesh.buf").write_bytes(struct.pack("<6I", 0, 1, 2, 0, 0, 0))
                    text(mod / "green.hlsl", "float4 main():SV_Target { return float4(0,1,0,1); }\n")
                    ini = f"""[ShaderOverrideVS]
hash = {vs}
checktextureoverride = ib

[TextureOverrideMesh]
match_type = Buffer
match_byte_width = 24
handling = skip
ib = ResourceMesh
"""
                    if mode.startswith("scoped"):
                        ini += f"if {int(mode == 'scoped')}\n run = CustomShaderScoped\nelse\n drawindexed = 3, 0, 0\nendif\n"
                        ini += "\n[CustomShaderScoped]\nps = green.hlsl\ndrawindexed = 3, 0, 0\n"
                    else:
                        ini += "drawindexed = 3, 0, 0\n"
                    if mode == "late":
                        ini += f"\n[ShaderOverridePS]\nhash = {ps}\nhandling = skip\nrun = CustomShaderLate\n\n[CustomShaderLate]\nps = green.hlsl\ndraw = from_caller\n"
                    ini += "\n[ResourceMesh]\ntype = Buffer\nformat = R32_UINT\nfilename = mesh.buf\n"
                    text(mod / "mod.ini", ini)
            raw = execute(exe, [api, "none" if mode == "native" else directory / "d3d11.dll", shaders], directory)
            actual = json.loads(raw)
            expected = dict(red=0 if mode == "scoped" else 256,
                            green=256 if mode == "scoped" else 0, black=0, other=0)
            if actual != expected:
                raise AssertionError(f"{api}/{mode}: {actual}, expected {expected}")
            if mode != "native":
                log = (directory / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace")
                if "Unrecognised entry:" in log or "Error compiling" in log:
                    raise AssertionError(f"{directory.name}: invalid configuration or shader")
            notes = []
            if api == "dx12" and mode != "native":
                notes = [line for line in (directory / "dx12_report.txt").read_text(
                    encoding="utf-8", errors="replace").splitlines() if line.startswith("Mod draw:")]
                if mode == "scoped" and (not notes or any("3 from 3 indexed" in line for line in notes)):
                    raise AssertionError("scoped shader unexpectedly replayed the parent draw")
            late_draws = check_late_draw_order(api, log, notes) if mode == "late" else []
            cases.append(dict(api=api, mode=mode, pixels=actual, notes=notes,
                              late_draws=late_draws, passed=True))
            print(f"{api}/{mode}: PASS {actual}", flush=True)
    cases.append(check_constant_reads_dx11(exe, dll, dependencies, output, shaders, vs))
    cases.append(check_constant_reads(exe, dll, dependencies, output, shaders, vs))
    summary = dict(passed=True, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(),
                   vs_hash=vs, ps_hash=ps, cases=cases)
    text(output / "results.json", json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
