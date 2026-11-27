"""Exercise the D3D11 shim's copy, view, and descriptor paths on D3D12 WARP."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from check import execute, fnv64, text


def check_draw_srv_range(exe, dll_name, dxc, directory, ini):
    for stage, source in (
        ("vs", "float4 main(uint id : SV_VertexID) : SV_Position {\n"
               "    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0.5, 1);\n}\n"),
        ("ps", "float4 main() : SV_Target { return float4(1, 0, 0, 1); }\n"),
    ):
        shader = directory / f"{stage}.hlsl"
        text(shader, source)
        execute(dxc, ["-T", f"{stage}_6_0", "-E", "main", "-Fo", directory / f"{stage}.bin", shader], directory)
    pixel_hash = fnv64((directory / "ps.bin").read_bytes())
    text(directory / "overflow.hlsl", """#if __SHADER_TARGET_MAJOR >= 6
Texture2D<float4> images[2] : register(t127);
float4 main(float4 pos : SV_Position) : SV_Target
{
    return images[uint(pos.x) & 1].Load(int3(0, 0, 0));
}
#else
float4 main() : SV_Target { return float4(0, 1, 0, 1); }
#endif
""")
    text(directory / "d3dx.ini", ini + f"""shader_hash = 3dmigoto
[Hunting]
hunting = 0
dx12_dump_hashes = true
[ShaderOverridePS]
hash = {pixel_hash}
handling = skip
run = CustomShaderOverflow
[CustomShaderOverflow]
ps = overflow.hlsl
draw = from_caller
""")
    result = execute(exe, ["dx12", directory / dll_name, directory], directory)
    report = (directory / "dx12_report.txt").read_text(encoding="utf-8", errors="replace")
    if "Creating a pipeline of the game again with a mod's ps shader failed" in report:
        raise RuntimeError("DrawPath tried to create a pipeline with an unsupported SRV range")
    log = (directory / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace")
    draws = [line for line in report.splitlines() if line.startswith("Mod draw:")]
    if ("Unsupported SRV range in mod ps shader: images at t127, count 2" not in log
            or len(draws) != 1 or "(the mod's could not be)" not in draws[0]
            or json.loads(result) != dict(red=256, green=0, black=0, other=0)):
        raise RuntimeError("Expected DrawPath to reject the DXIL shader and draw with the game shader")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("exe", "dll", "dxc", "output"):
        parser.add_argument(f"--{argument}", type=Path, required=True)
    args = parser.parse_args()
    exe, dll, dxc, output = (path.resolve() for path in (args.exe, args.dll, args.dxc, args.output))
    paths = [exe, dll, dll.with_suffix(".pdb"), dxc, dll.parent / "dxcompiler.dll", dll.parent / "dxil.dll"]
    for path in paths:
        if not path.is_file():
            parser.error(f"missing file: {path}")
    output.mkdir(parents=True, exist_ok=False)

    source = Path(__file__).with_name("shim.hlsl").resolve()
    dxbc, dxil = (output / f"shim.{suffix}" for suffix in ("dxbc", "dxil"))
    execute(exe, ["compile-shim", source, dxbc], output)
    execute(dxc, ["-T", "cs_6_0", "-E", "main", "-Fo", dxil, source], output)
    stripped = output / "shim-stripped.dxil"
    execute(dxc, ["-T", "cs_6_0", "-E", "main", "-Qstrip_reflect", "-Fo", stripped, source], output)
    indexed = output / "shim-indexed.dxil"
    execute(dxc, ["-T", "cs_6_0", "-E", "indexed", "-D", "DXIL_INDEXED=1", "-Fo", indexed, source], output)
    ray_query, overflow, unbounded = (output / f"shim-{name}.dxil" for name in ("ray-query", "overflow", "unbounded"))
    for shader, target, define in (
        (ray_query, "cs_6_5", "DXIL_RAY_QUERY"),
        (overflow, "cs_6_0", "DXIL_SRV_OVERFLOW"),
        (unbounded, "cs_6_0", "DXIL_SRV_UNBOUNDED"),
    ):
        execute(dxc, ["-T", target, "-E", "main", "-D", define, "-Fo", shader, source], output)

    failures = []
    for name, mode, shader in (
        ("copy", "copy", dxbc),
        ("texture-replay", "texture-replay", dxbc),
        ("game-upload", "game-upload", dxbc),
        ("depth", "depth", dxbc),
        ("bindings-dxbc", "bindings", dxbc),
        ("bindings-dxil", "bindings", dxil),
        ("bindings-indexed", "bindings-indexed", indexed),
        ("bindings-ray-query", "bindings-ray-query", ray_query),
        ("bindings-overflow", "bindings-unsupported", overflow),
        ("bindings-unbounded", "bindings-unsupported", unbounded),
        ("registry-dxil", "registry", stripped),
        ("queue-lifetime", "queue-lifetime", dxbc),
        ("queue-budget", "queue-budget", dxbc),
        ("present-queues", "present-queues", dxbc),
        ("removed-resize", "removed-resize", dxbc),
        ("draw-srv-overflow", "draw-srv-overflow", None),
    ):
        directory = output / name
        directory.mkdir()
        for path in paths[1:3] + paths[4:]:
            shutil.copy2(path, directory / path.name)
        ini = f"""[Loader]
target = {exe.name}
require_admin = false
[System]
check_foreground_window = false
[Logging]
log_level = info
unbuffered = true
[Rendering]
extended_rendering = true
"""
        try:
            if mode == "draw-srv-overflow":
                result = check_draw_srv_range(exe, dll.name, dxc, directory, ini)
            else:
                text(directory / "d3dx.ini", ini)
                result = execute(exe, [f"shim-{mode}", directory / dll.name, shader], directory)
            if mode in ("queue-budget", "bindings-ray-query", "bindings-unsupported"):
                log = (directory / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace")
            if mode == "queue-budget":
                blocked = set(re.findall(r"ID3D12CommandQueue (\w+) (?:cannot safely use|cannot be made to wait)", log))
                if len(blocked) != 6:
                    raise RuntimeError(f"expected 6 blocked queues, got {len(blocked)}")
            if mode == "bindings-ray-query":
                if "Null raytracing acceleration structure SRVs require a hardware adapter." not in log:
                    raise RuntimeError("missing WARP acceleration structure diagnostic")
            if mode == "bindings-unsupported":
                if "Unsupported SRV range in mod cs shader: images at t127" not in log:
                    raise RuntimeError("missing unsupported SRV range diagnostic")
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            stderr = directory / "stderr.log"
            detail = stderr.read_text(encoding="utf-8", errors="replace").strip() if stderr.exists() else ""
            failures.append(name)
            print(f"{name}: FAIL {detail or error}", flush=True)
        else:
            detail = result.decode("utf-8", errors="replace").strip()
            print(f"{name}: PASS" + (f" ({detail})" if detail else ""), flush=True)
    if failures:
        raise SystemExit(f"shim regression: {', '.join(failures)}")


if __name__ == "__main__":
    main()
