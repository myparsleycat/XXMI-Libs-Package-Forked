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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True,
                        help="Directory containing dxcompiler.dll and dxil.dll")
    parser.add_argument("--output", type=Path, required=True,
                        help="New, empty output directory (never overwritten)")
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
    for api in ("dx11", "dx12"):
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
    summary = dict(passed=True, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(),
                   vs_hash=vs, ps_hash=ps, cases=cases)
    text(output / "results.json", json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
