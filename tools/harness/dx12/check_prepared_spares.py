"""Check background spare use and buffer contents across two D3D12 queues."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HARNESS = Path(__file__).resolve().parent
REPO = HARNESS.parents[2]
CAPTURE_FRAMES = [99, 100, 299, 300, 301, 639]


def run_case(run, dll, async_loading):
    run.mkdir()
    shutil.copy2(dll, run / "d3d11.dll")
    shutil.copy2(HARNESS / "bin/harness.exe", run)
    for name in ("d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll", "SPIRV-Tools-shared.dll"):
        shutil.copy2(REPO / "Dependencies" / name, run)
    shutil.copytree(HARNESS / "case/Mods", run / "Mods")
    subprocess.run(
        [sys.executable, str(HARNESS.parent / "common/make_ini.py"), str(run / "d3dx.ini"),
         "System.check_foreground_window=0", "Hunting.dx12_dump_hashes=1",
         f"Rendering.async_resource_loading={int(async_loading)}"],
        check=True,
    )
    mod = run / "Mods/mod/mod.ini"
    ini = mod.read_text(encoding="utf-8")
    section = re.search(r"\[TextureOverrideMesh100\][\s\S]*?(?=\n\[)", ini)
    assert section and "vb0 = ResourceModPositions" in section[0]
    # Keep using the first buffer for several frames before overwriting it on each draw.
    code = section[0].replace("vb0 = ResourceModPositions", """if ResourceSidePositions === null || $spare_test_frame >= 100
    ResourceSidePositions = copy ResourceModPositions
endif
vb0 = ref ResourceSidePositions""")
    mod.write_text(ini[:section.start()] + code + ini[section.end():] + """
[ResourceSidePositions]

[Constants]
global $spare_test_frame = 0

[Present]
$spare_test_frame = $spare_test_frame + 1
""", encoding="utf-8")
    result = subprocess.run(
        [str(run / "harness.exe"), "frames=640", "meshes=101", "passes=3", "side",
         "capture=" + ",".join(map(str, CAPTURE_FRAMES)), "dump=scene.bmp"],
        cwd=run, capture_output=True, text=True, check=True, timeout=65,
    )
    captures = re.findall(r"(?m)^capture (\d+):\npicture checksum ([a-f0-9]+)$", result.stdout)
    assert [int(frame) for frame, _ in captures] == CAPTURE_FRAMES, result.stdout
    report = (run / "dx12_report.txt").read_text(encoding="utf-8", errors="replace")
    counts = re.findall(
        r"DX12 buffer replacements used (\d+) prepared buffer spares and made (\d+) synchronous allocation attempts",
        report,
    )
    return captures, sum(int(prepared) for prepared, _ in counts), sum(int(sync) for _, sync in counts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, default=REPO / "x64/Release/d3d11.dll")
    dll = parser.parse_args().dll.resolve()
    with tempfile.TemporaryDirectory(prefix="xxmi-prepared-spares-") as root:
        root = Path(root)
        checksum, hits, fallback = run_case(root / "enabled", dll, True)
        assert hits, "DX12 buffer replacements did not use a prepared spare"
        disabled_checksum, disabled_hits, disabled_fallback = run_case(root / "disabled", dll, False)
        assert disabled_hits == 0 and disabled_fallback > 0
        assert checksum == disabled_checksum, (checksum, disabled_checksum)
        print(f"PASS: {hits} prepared spares, {fallback} synchronous allocation attempts "
              f"({disabled_fallback} with async disabled), matching rendering at {len(checksum)} frames")


if __name__ == "__main__":
    main()

