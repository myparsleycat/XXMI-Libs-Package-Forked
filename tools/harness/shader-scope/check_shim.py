"""Exercise the D3D11 shim's copy, view, and descriptor paths on D3D12 WARP."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess

from check import execute, text


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

    failures = []
    for name, mode, shader in (
        ("copy", "copy", dxbc),
        ("depth", "depth", dxbc),
        ("bindings-dxbc", "bindings", dxbc),
        ("bindings-dxil", "bindings", dxil),
        ("registry-dxil", "registry", stripped),
        ("queue-lifetime", "queue-lifetime", dxbc),
        ("queue-budget", "queue-budget", dxbc),
        ("present-queues", "present-queues", dxbc),
        ("removed-resize", "removed-resize", dxbc),
    ):
        directory = output / name
        directory.mkdir()
        for path in paths[1:3] + paths[4:]:
            shutil.copy2(path, directory / path.name)
        text(directory / "d3dx.ini", f"""[Loader]
target = {exe.name}
require_admin = false
[System]
check_foreground_window = false
[Logging]
log_level = info
unbuffered = true
[Rendering]
extended_rendering = true
async_resource_loading = false
""")
        try:
            result = execute(exe, [f"shim-{mode}", directory / dll.name, shader], directory)
            if mode == "queue-budget":
                log = (directory / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace")
                blocked = set(re.findall(r"ID3D12CommandQueue (\w+) (?:cannot safely use|cannot be made to wait)", log))
                if len(blocked) != 6:
                    raise RuntimeError(f"expected 6 blocked queues, got {len(blocked)}")
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
