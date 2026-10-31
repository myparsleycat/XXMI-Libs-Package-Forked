import argparse
import re
import subprocess
import sys
from pathlib import Path


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(folder, seconds):
    result = subprocess.run(
        [str(folder / "harness.exe"), "--pool", "--bonetick", "--frames", "60",
         "--min-seconds", str(seconds)],
        cwd=folder, capture_output=True, text=True, timeout=60,
    )
    require(result.returncode == 0 and "FAILED" not in result.stdout,
            f"Vulkan harness failed: {result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder", type=Path, help="isolated folder prepared by vk/setup.sh")
    args = parser.parse_args()
    folder = args.folder.resolve()
    require((folder / "harness.exe").is_file(), "Run vk/setup.sh for the test folder first")
    ini = folder / "d3dx.ini"
    mod = folder / "Mods" / "test.ini"
    log = folder / "d3d11_spike_log.txt"
    require(not log.exists(), "Use a fresh run folder without an existing spike log")
    original_ini, original_mod = ini.read_bytes(), mod.read_bytes()
    maker = Path(__file__).resolve().parent.parent / "common" / "make_ini.py"
    template = Path(__file__).resolve().parent / "case" / "tests" / "test_compute.ini"
    settings = [
        "Loader.target=harness.exe", "Loader.require_admin=false",
        "System.check_foreground_window=0", "Rendering.track_region_hashes=1",
        "Hunting.vk_dump_hashes=0", "Logging.log_level=disabled",
        "Logging.spike_log_relative=0", r"Logging.spike_log_mod_state=$\SpikeTest\enable_mods",
    ]
    try:
        subprocess.run([sys.executable, str(maker), str(ini), *settings,
                        "Logging.spike_log_threshold=1000"], check=True)
        mod.write_text("namespace = SpikeTest\n" + template.read_text(encoding="utf-8") + """
[Constants]
global $test_started = -1

[Present]
if $test_started < 0
    $test_started = time
endif
local $elapsed = time - $test_started
if $elapsed >= 0.8 && $elapsed < 1.6
    $enable_mods = 0
else
    $enable_mods = 1
endif
""", encoding="utf-8")
        run(folder, 7)
        require(log.is_file(), "Enabled spike logging did not produce a file")
        text = log.read_text(encoding="utf-8-sig")
        states = re.findall(r"observed mod state: (ON|OFF); shim ON; \$\\SpikeTest\\enable_mods=(ON|OFF)", text)
        require(states == [("ON", "ON"), ("OFF", "OFF"), ("ON", "ON")],
                f"Configured mod state did not follow ON/OFF/ON: {states}")
        summaries = re.findall(r"summary of [\d.]+s with mods (on|OFF):", text)
        require(summaries == ["on", "OFF", "on"],
                f"Mod transitions did not split summary windows: {summaries}")
        require("Nested per-frame measurements (inclusive; may overlap):" in text,
                "Nested times were not identified as inclusive")
        for category in ("constant-buffer lookup", "shader-resource lookup", "publishing a game buffer",
                         "index-region hash lookup", "preparing shadow uploads", "recording mod draws",
                         "command list execution"):
            require(category in text, f"Missing detailed timing: {category}")
        byte_counts = re.findall(r"bytes/frame: render ([\d.]+), other ([\d.]+)", text)
        require(byte_counts and any(float(a) + float(b) > 0 for a, b in byte_counts),
                "Buffer processing did not report bytes per frame")
        steady = text.rsplit("summary of 5.0s with mods on:", 1)
        require(len(steady) == 2, "No complete steady-state summary was recorded")
        frames = int(re.match(r" (\d+) frames", steady[1]).group(1))
        refreshed = re.search(
            r"Vulkan: reading and comparing shadow pages[^\n]*\n\s+bytes/frame: render ([\d.]+), other ([\d.]+)",
            steady[1],
        )
        require(refreshed is not None, "Refresh workers did not report processed bytes")
        processed = sum(map(float, refreshed.groups()))
        require(abs(processed - (8 << 20)) <= 2 * (8 << 20) / frames + 0.1,
                f"Concurrent refresh accounting did not preserve the 8 MiB buffer size: {processed}")
        require(not (folder / "vk_report.txt").exists() and not (folder / "vk_shaders").exists(),
                "Spike measurements enabled Vulkan hash dumping")
        require("command list self time" not in text, "Self times were measured without being asked for")
        log.rename(folder / "spike-enabled.txt")
        subprocess.run([sys.executable, str(maker), str(ini), *settings,
                        "Logging.spike_log_threshold=1000", "Logging.spike_log_command_detail=1"], check=True)
        run(folder, 7)
        detail = log.read_text(encoding="utf-8-sig").rsplit("summary of 5.0s with mods on:", 1)
        require(len(detail) == 2, "No complete summary with self times was recorded")
        metric = r"\s+([\d.]+)ms \(x[\d.]+\s*\)\s+([\d.]+)ms \(x[\d.]+\s*\)\s+"
        total = re.search(metric + r"command list execution", detail[1])
        parts = re.findall(metric + r"command list self time", detail[1])
        require(total is not None and len(parts) > 1, "Self times were not reported")
        for column in (0, 1):
            whole, added = float(total[column + 1]), sum(float(part[column]) for part in parts)
            require(abs(whole - added) <= 0.15 * whole + 0.01,
                    f"Self times add up to {added} ms of {whole} ms")
        log.rename(folder / "spike-detail.txt")
        subprocess.run([sys.executable, str(maker), str(ini), *settings,
                        "Logging.spike_log_threshold=0"], check=True)
        run(folder, 0.2)
        require(not log.exists(), "Disabled spike logging still produced a file")
        print("PASS: timing and byte summaries without hash dumps, ON/OFF/ON window boundaries, self times adding up, logging disabled")
    finally:
        ini.write_bytes(original_ini)
        mod.write_bytes(original_mod)


if __name__ == "__main__":
    main()
