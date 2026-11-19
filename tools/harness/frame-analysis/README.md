# DX12 frame-analysis window

The hidden WARP program invokes the real frame-analysis key handler through
an explicitly test-only DLL. It sends no keyboard events and starts no game.
The synthetic PS reads a known 16x16 red texture. CPU readback and each
captured DDS must agree byte-for-byte.

## Build and run (PowerShell)

Requires Visual Studio 2022 C++ tools, Windows SDK and Python 3.12+.
The builder defaults to BuildTools; pass --vs for another edition.

~~~powershell
python tools/harness/frame-analysis/build.py D:/xxmi-tests/capture-build
python tools/harness/frame-analysis/check.py D:/xxmi-tests/capture-build D:/xxmi-runtime D:/xxmi-tests/capture-results
~~~

Both output directories must be new. The runtime directory supplies the
release's d3dcompiler_47.dll, dxcompiler.dll and dxil.dll; it is not modified.
Output, objects and logs go outside the checkout. Do not use Python -O.

The builder imports a temporary MSBuild props file which substitutes a
generated translation unit for Hunting.cpp. That unit includes the unchanged
Hunting.cpp, then exports the real handler and its armed state. Projects use
distinct intermediate directories. No test hook, build property or export
is added to normal builds. Never install the test-only DLL in a game.

## Cases and reproduction

Invoke the handler after the initial Present, then insert 0 through 6
Presents without a draw before recording a textured DrawIndexedInstanced.
--only-gap 1 is the minimal regression: unmodified nhd.32 renders correctly
but leaves an empty capture directory. With the fix, gaps 0 through 4
produce log.txt and two names of the same DDS; gaps 5 and 6 produce only
capture_status.txt and do not capture the later draw. The state hook checks
the exact disarm boundary.

Other cases abort a pending request, retry after timeout, and request only
a log. All check the final 256 red pixels. The no-gap DDS is the
before/after byte-equivalence control.

This checks deterministic capture timing, not keyboard polling, game
protection, arbitrary output permissions, or all multi-queue engines.
It adds neither VB/IB export nor Shader modifications.
