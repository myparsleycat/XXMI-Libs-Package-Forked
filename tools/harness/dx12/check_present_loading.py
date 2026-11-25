"""Check first-match loading and deferred initialization with the D3D12 harness."""
import argparse
from datetime import datetime
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
ResourceEarlyIntermediate = copy ResourceEarlyInput
ResourceEarlyOutput = copy ResourceEarlyIntermediate

[ResourceCrossOutput]

[ResourceCrossInput]
type = Buffer
format = R32_UINT
filename = cross.buf

[ResourceEarlyInput]
type = RWBuffer
format = R32G32B32A32_FLOAT
array = 1536

[ResourceEarlyIntermediate]

[ResourceEarlyOutput]
byte_width = 16384
"""

PRESSURE_PEER = """
namespace = peer
[Constants]
global $object_detected = 0

[TextureOverridePeer]
hash = a3b1abf1
allow_duplicate_hash = true
match_priority = 20
$object_detected = 0

[Present]
if $object_detected
    ResourcePeerOutput = copy ResourcePeerInput
endif

[ResourcePeerInput]
type = Buffer
format = R32_UINT
array = 8192

[ResourcePeerOutput]
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


def run_probe(folder, dll, detect, async_loading, flip=False, spike_threshold=0.01, redetection=False,
              spare_pressure=False, continuous=False, nested_activation=False, vram_budget_percent=75):
    mods = folder / "Mods"
    mods.mkdir(parents=True)
    shutil.copy2(dll, folder / "d3d11.dll")
    shutil.copy2(HARNESS / "bin/harness.exe", folder)
    for name in ("d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll", "SPIRV-Tools-shared.dll"):
        shutil.copy2(REPO / "Dependencies" / name, folder)
    subprocess.run(
        [sys.executable, str(HARNESS.parent / "common/make_ini.py"), str(folder / "d3dx.ini"),
         "System.check_foreground_window=0", f"Rendering.async_resource_loading={async_loading}",
         f"Rendering.dx12_evict_budget_percent={vram_budget_percent}",
         "Rendering.async_resource_commit_mb=1", "Logging.log_level=info",
         f"Logging.spike_log_threshold={spike_threshold}", "Logging.spike_log_relative=0"],
        check=True,
    )
    shutil.copy2(HARNESS / "case/Mods/harness.ini", mods)
    owner = OWNER.format(detect=detect)
    cross = CROSS
    if nested_activation:
        cross = cross.replace("[Present]\n", "[Present]\nrun = CommandListActivate\n\n[CommandListActivate]\n")
    if spare_pressure:
        owner = owner.replace("hash = a3b1abf1",
                              "hash = a3b1abf1\nallow_duplicate_hash = true\nmatch_priority = -20")
        (mods / "peer.ini").write_text(PRESSURE_PEER, encoding="utf-8")
        fillers = range(16)
        copies = "".join(f"ResourceFill{n} = copy ResourceEarlyInput\n" for n in fillers)
        cross = cross.replace("[CommandListPrepare]\n", "[CommandListPrepare]\n" + copies)
        cross += "".join(f"\n[ResourceFill{n}]\nbyte_width = {256 * (n + 2)}\n" for n in fillers)
    if redetection:
        owner = owner.replace("hash = a3b1abf1", "hash = a3b1abf1\nallow_duplicate_hash = true")
        owner = owner.replace("[CommandListRegister]", r"""
if $ticks >= 420
    if Resource\cross\EarlyIntermediate === null || ($ticks >= 500 && $ticks < 530)
        run = CommandList\cross\CopyEarlyIntermediate
    endif
    vb0 = ref Resource\cross\EarlyIntermediate
endif

[CommandListRegister]""")
        cross = cross.replace(r"$\probe\object_detected)", r"$\probe\object_detected || $\peer\object_detected)")
        copy = "ResourceEarlyIntermediate = copy ResourceEarlyInput"
        cross = cross.replace(copy, "run = CommandListCopyEarlyIntermediate")
        cross += "\n[CommandListCopyEarlyIntermediate]\n" + copy + "\n"
        (mods / "peer.ini").write_text("""
namespace = peer
[Constants]
global $object_detected = 0
[TextureOverridePeer]
hash = a3b1abf1
allow_duplicate_hash = true
$object_detected = 0
""", encoding="utf-8")
    (mods / "probe.ini").write_text(owner, encoding="utf-8")
    (mods / "cross.ini").write_text(cross, encoding="utf-8")
    if flip:
        (mods / "flip.ini").write_text(FLIP, encoding="utf-8")
        (mods / "big.buf").write_bytes(b"BIG!" * (2 << 20))
    (mods / "own.buf").write_bytes(b"OWN!" * (2 << 20))
    (mods / "cross.buf").write_bytes(b"XROS" * (2 << 20))
    args = [str(folder / "harness.exe"), "meshes=101"]
    args.extend(["frames=640", "passes=3", "side"] if redetection else ["frames=240", "passes=1"])
    if redetection:
        args.append("pause=120,12000")
    elif continuous or spare_pressure:
        args.extend(["frames=6000", "min-ms=7000"])
    result = subprocess.run(args, cwd=folder, check=True, timeout=120, capture_output=True, text=True)
    log = (folder / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace").lower()
    spike = (folder / "d3d11_spike_log.txt").read_text(encoding="utf-8-sig")
    return log, spike, result.stdout


def check_redetection(spike, output):
    assert not re.search(r"(?m)^\[.*\] frame \d+:", spike), "probe logged a slow frame"
    resumed = re.search(r"(?m)^resumed \[([^\]]+)\]$", output)
    assert resumed, output
    events = [match.groupdict() for match in re.finditer(
        r"(?m)^\[(?P<time>[^\]]+)\] DX12 spare buffer: (?P<event>\w+) id=(?P<id>\d+) width=0x6000 flags=0x0 "
        r"status=(?P<status>\w+)(?: mod=(?P<mod>\w+))?$", spike,
    )]
    time_format = "%H:%M:%S.%f"
    resumed_at = datetime.strptime(resumed[1], time_format)

    def after_resume(event):
        elapsed = (datetime.strptime(event["time"], time_format) - resumed_at).total_seconds()
        return elapsed % (24 * 60 * 60) < 12 * 60 * 60

    initial = [event for event in events if event["status"] == "queued_first_match"]
    renewed = [event for event in events if event["status"] == "queued_redetection"]
    initial_ids = {event["id"] for event in initial}
    renewed_ids = {event["id"] for event in renewed}
    assert len(initial_ids) == len(initial) == 2, events
    assert len(renewed_ids) == len(renewed) == 2 and initial_ids.isdisjoint(renewed_ids), events
    assert all(after_resume(event) for event in renewed), events
    for spare_id in initial_ids:
        lifecycle = [event for event in events if event["id"] == spare_id and event["event"] != "requested"]
        assert [event["event"] for event in lifecycle] == ["ready", "expired"], events
        assert lifecycle[-1]["status"] == "idle_timeout" and not after_resume(lifecycle[-1]), events
    for spare_id in renewed_ids:
        assert {event["mod"] for event in events if event["id"] == spare_id and event["event"] == "requested"} == {
            "probe", "peer"
        }, events
        lifecycle = [event["event"] for event in events if event["id"] == spare_id and event["event"] != "requested"]
        assert lifecycle in (["ready"], ["ready", "consumed"]), events
    assert any(event["event"] == "consumed" and event["id"] in renewed_ids for event in events), (
        "a live buffer replacement must use a spare prepared on redetection",
        [event for event in events if event["id"] in renewed_ids or event["event"] == "consumed"],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, default=REPO / "x64/Release/d3d11.dll")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="xxmi-present-loading-") as root:
        root = Path(root)
        dormant, spike, _ = run_probe(root / "dormant", args.dll.resolve(), detect=1000, async_loading=1)
        uploaded = sum(map(int, re.findall(
            r"recording split mod uploads at Present .*\n\s+processed (\d+) bytes", spike
        )))
        assert uploaded == 16 << 20, (
            "first match must stage both 8 MiB files while activation flags are still false"
        )
        assert "creating cached resource" not in dormant, "inactive initialization ran"
        early_spares = re.findall(
            r"prepared a \[present\] replacement buffer on first match, width=([0-9a-f]+)",
            dormant,
        )
        assert {0x6000, 0x4000} <= {int(width, 16) for width in early_spares}, (
            "first match must prepare both cross-namespace copy destinations before activation", early_spares
        )

        _, quiet_spike, output = run_probe(root / "quiet", args.dll.resolve(), detect=1000, async_loading=1,
                                           spike_threshold=100000, redetection=True)
        check_redetection(quiet_spike, output)

        _, pressure_spike, _ = run_probe(root / "pressure", args.dll.resolve(), detect=2**31, async_loading=1,
                                         spike_threshold=100000, spare_pressure=True)
        for width in (0x4000, 0x8000):
            statuses = re.findall(
                rf"DX12 spare buffer: \w+ id=\d+ width=0x{width:x} flags=0x0 status=(\w+)", pressure_spike,
            )
            assert statuses == ["slot_limit", "queued_first_match", "queued_first_match", "created", "created"], (
                "each mod's deferred descriptor must get slots before expired descriptors are refilled",
                hex(width), statuses,
            )
        assert "status=capacity" in pressure_spike and "status=queued_redetection" not in pressure_spike, (
            "unused spares must make room for deferred requests without being queued again", pressure_spike,
        )

        _, continuous_spike, _ = run_probe(root / "continuous", args.dll.resolve(), detect=2**31, async_loading=1,
                                           spike_threshold=100000, continuous=True)
        assert "status=created" in continuous_spike and not re.search(
            r"DX12 spare buffer: .*\b(?:expired|queued_redetection)\b", continuous_spike
        ), (
            "continuously drawn dormant mods must keep their ready spares when there is room", continuous_spike,
        )

        _, vram_spike, _ = run_probe(root / "vram", args.dll.resolve(), detect=1000, async_loading=1,
                                    spike_threshold=100000, vram_budget_percent=0)
        pressure_event = re.search(r"DX12 spare buffer: .*status=vram_(?:pressure|limit)\b", vram_spike)
        assert pressure_event and "status=created" not in vram_spike[pressure_event.end():], (
            "the spare pool must stop preparing resident buffers once VRAM usage exceeds the limit", vram_spike,
        )

        active, _, _ = run_probe(root / "active", args.dll.resolve(), detect=0, async_loading=1, flip=True)
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

        nested, nested_spike, _ = run_probe(root / "nested", args.dll.resolve(), detect=0, async_loading=1,
                                           nested_activation=True)
        assert re.search(r"DX12 spare buffer: requested .*width=0x6000 flags=0x0 status=queued_first_match mod=probe",
                         nested_spike), "first match missed the buffer copy under an explicit [Present] run"
        assert any(" resourcecrossoutput = copy" in line and "creating cached resource" in line
                   for line in nested.splitlines()), "the nested initializer never ran"
        assert not any("loaded on use:" in line and "crossinput" in line for line in nested.splitlines()), (
            "the nested initializer loaded its input synchronously"
        )

        disabled, spike, _ = run_probe(root / "disabled", args.dll.resolve(), detect=0, async_loading=0)
        assert "recording split mod uploads at Present" not in spike
        assert "prepared a [present] replacement buffer on first match" not in disabled
        assert all(any("creating cached resource" in line and f" {name} = copy" in line
                       for line in disabled.splitlines())
                   for name in ("resourcecrossoutput", "resourceownoutput"))
    print("PASS: early preparation, idle expiry and redetection, sustained drawing, slot and VRAM pressure, "
          "explicit [Present] runs, deferred initialization, registration, post reset, pre/post agreement and async disabled")


if __name__ == "__main__":
    main()

