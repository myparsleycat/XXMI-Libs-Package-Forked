"""Check first-match loading and deferred initialization with the D3D12 harness."""
import argparse
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HARNESS = Path(__file__).resolve().parent
REPO = HARNESS.parents[2]

OWNER = """
namespace = probe
[Constants]
global $object_detected = 0
global $mod_enabled = 0
global $ticks = 0
global $saw_reset = 0

[Present]
$ticks = $ticks + 1
if $object_detected
    if $mod_enabled
        post $object_detected = 0
        run = CommandListPrepare
    else
        run = CommandListRegister
    endif
endif

[TextureOverrideMesh100]
hash = a3b1abf1
if $mod_enabled && !$object_detected && !$saw_reset
    ResourceReset = copy ResourceMarker
    $saw_reset = 1
endif
if $ticks >= {detect}
    $object_detected = 1
endif

[CommandListRegister]
ResourceRegistration = copy ResourceMarker
$mod_enabled = 1

[CommandListPrepare]
local $initialized
if !$initialized
    ResourceOwnOutput = copy ResourceOwnInput
    $initialized = 1
endif

[ResourceMarker]
type = Buffer
format = R32_UINT
data = 1

[ResourceRegistration]
[ResourceReset]
[ResourceOwnOutput]

[ResourceOwnInput]
type = Buffer
format = R32_UINT
filename = own.buf
"""

CROSS = r"""
namespace = cross
[Present]
local $initialized
if ($\probe\mod_enabled || $\probe\object_detected) && !$initialized
    run = CommandListPrepare
    $initialized = 1
endif

[CommandListPrepare]
ResourceCrossOutput = copy ResourceCrossInput

[ResourceCrossOutput]

[ResourceCrossInput]
type = Buffer
format = R32_UINT
filename = cross.buf
"""

# The pre commands change the variable that picks the branch with a file. The
# post commands of that draw must still run: a pre run whose post was skipped
# leaves $pending set, which the next pre run sees.
FLIP = """
namespace = flip
[Constants]
global $flip = 0
global $pending = 0

[TextureOverrideFlip]
hash = a3b1abf1
if $pending
    ResourceMismatch = copy ResourceFlipMarker
endif
$pending = 1
if $flip
    ResourceBigOut = copy ResourceBig
endif
$flip = 1
post $pending = 0

[ResourceFlipMarker]
type = Buffer
format = R32_UINT
data = 1

[ResourceMismatch]
[ResourceBigOut]

[ResourceBig]
type = Buffer
format = R32_UINT
filename = big.buf
"""


def run_probe(folder, dll, detect, async_loading, flip=False):
    mods = folder / "Mods"
    mods.mkdir(parents=True)
    shutil.copy2(dll, folder / "d3d11.dll")
    shutil.copy2(HARNESS / "bin/harness.exe", folder)
    for name in ("d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll", "SPIRV-Tools-shared.dll"):
        shutil.copy2(REPO / "Dependencies" / name, folder)
    subprocess.run(
        [sys.executable, str(HARNESS.parent / "common/make_ini.py"), str(folder / "d3dx.ini"),
         "System.check_foreground_window=0", f"Rendering.async_resource_loading={async_loading}",
         "Rendering.async_resource_commit_mb=1", "Logging.log_level=info",
         "Logging.spike_log_threshold=0.01"],
        check=True,
    )
    shutil.copy2(HARNESS / "case/Mods/harness.ini", mods)
    (mods / "probe.ini").write_text(OWNER.format(detect=detect), encoding="utf-8")
    (mods / "cross.ini").write_text(CROSS, encoding="utf-8")
    if flip:
        (mods / "flip.ini").write_text(FLIP, encoding="utf-8")
        (mods / "big.buf").write_bytes(b"BIG!" * (2 << 20))
    (mods / "own.buf").write_bytes(b"OWN!" * (2 << 20))
    (mods / "cross.buf").write_bytes(b"XROS" * (2 << 20))
    subprocess.run([str(folder / "harness.exe"), "frames=240", "meshes=101", "passes=1"],
                   cwd=folder, check=True, timeout=60)
    log = (folder / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace").lower()
    spike = (folder / "d3d11_spike_log.txt").read_text(encoding="utf-8-sig")
    return log, spike


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, default=REPO / "x64/Release/d3d11.dll")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="xxmi-present-loading-") as root:
        root = Path(root)
        dormant, spike = run_probe(root / "dormant", args.dll.resolve(), detect=1000, async_loading=1)
        uploaded = sum(map(int, re.findall(
            r"recording split mod uploads at Present .*\n\s+processed (\d+) bytes", spike
        )))
        assert uploaded == 16 << 20, (
            "first match must stage both 8 MiB files while activation flags are still false"
        )
        assert "creating cached resource" not in dormant, "inactive initialization ran"

        active, _ = run_probe(root / "active", args.dll.resolve(), detect=0, async_loading=1, flip=True)
        lines = [line for line in active.splitlines() if "creating cached resource" in line]
        registration, reset, cross, own = (
            next(i for i, line in enumerate(lines) if f" {name} = copy" in line)
            for name in ("resourceregistration", "resourcereset", "resourcecrossoutput", "resourceownoutput")
        )
        assert registration < reset < min(cross, own), "registration or post reset waited for initialization"
        assert not any("loaded on use:" in line and
                       ("owninput" in line or "crossinput" in line)
                       for line in active.splitlines()), "[Present] loaded a file synchronously"
        assert any(" resourcebigout = copy" in line for line in lines), "the flip branch never ran"
        assert not any(" resourcemismatch = copy" in line for line in lines), \
            "a TextureOverride ran its pre commands without its post commands"

        disabled, spike = run_probe(root / "disabled", args.dll.resolve(), detect=0, async_loading=0)
        assert "recording split mod uploads at Present" not in spike
        assert all(any("creating cached resource" in line and f" {name} = copy" in line
                       for line in disabled.splitlines())
                   for name in ("resourcecrossoutput", "resourceownoutput"))
    print("PASS: early loading, deferred initialization, registration, post reset, pre/post agreement and async disabled")


if __name__ == "__main__":
    main()

