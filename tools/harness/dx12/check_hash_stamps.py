import argparse
from pathlib import Path
import subprocess


def source_between(source, start, end=None):
    start = source.index(start)
    return source[start:source.index(end, start)] if end else source[start:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", help="Compile the production code from this Git ref")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[2]
    build = here / "bin" / "hash-stamps"
    build.mkdir(parents=True, exist_ok=True)

    def source(name):
        path = f"DirectX11/dx12/{name}"
        if args.baseline:
            return subprocess.check_output(
                ["git", "show", f"{args.baseline}:{path}"], cwd=repo, encoding="utf-8-sig"
            )
        return (repo / path).read_text(encoding="utf-8-sig")

    tracker = source("ResourceTracker.cpp")
    (build / "tracker.inc").write_text(
        source_between(tracker, "struct ResourceShard {", "// The resources with Evict")
        + source_between(tracker, "// The buffers found before"),
        encoding="utf-8",
    )
    (build / "texture.inc").write_text(
        source_between(
            source("TextureHash.cpp"),
            "static const double AT_ONCE_BUDGET_MS",
            "// With the hash of a pending texture",
        ),
        encoding="utf-8",
    )
    (build / "dred.inc").write_text(
        source_between(source("DeviceRemoved.cpp"), "static void note_game_buffer(", "static void note_allocations("),
        encoding="utf-8",
    )

    root = Path("..", "..", "..", "..", "..")
    crc = root / "crc32c-hw-1.0.5"
    msvc = Path("..", "..", "..", "common", "msvc.bat")
    fixture = Path("..", "..", "hash_stamps.cpp")
    subprocess.run(
        f'cmd.exe /d /c call {msvc} && '
        f'cl /nologo /O2 /EHsc /std:c++23 /Zc:__cplusplus /W3 /DCRC32C_STATIC=1 '
        f'/I. /I{root} /I{crc / "include"} '
        f'/Fe:hash_stamps.exe {fixture} {crc / "src" / "crc32c.cpp"}',
        cwd=build,
        check=True,
    )
    results = [
        subprocess.run([build / "hash_stamps.exe", case], cwd=build).returncode
        for case in ("va", "texture", "padded", "dred", "dred-overlap-va", "dred-overlap-serial")
    ]
    return int(any(results))


if __name__ == "__main__":
    raise SystemExit(main())
