# Shader callback scope

This hidden-window D3D11/D3D12 WARP harness checks command-list ordering and
`CustomShader` scope without a game, extracted shaders, textures, or meshes.
It does not implement automatic shader replacement.

Build from PowerShell or cmd, using the same MSVC setup as the other harnesses:

```bat
tools\harness\shader-scope\build.bat C:\temp\scope-build
```

Then run with a DLL and its DXC dependencies, into a **new** output directory:

```bat
python tools\harness\shader-scope\check.py --exe C:\temp\scope-build\harness.exe --dll C:\test-libs\d3d11.dll --dependencies C:\test-libs --output C:\temp\scope-results
```

The check copies the DLL/dependencies into isolated test folders. It does not
change the supplied DLL, installed importer, or global configuration. The
output directory must not already exist. It needs Python 3.12+, VS2022 C++,
the Windows SDK, and working D3D11/D3D12 WARP runtimes. No physical input is sent.

The original red triangle uses indices 3..5. A mesh override submits a red
triangle from indices 0..2 of its replacement buffer; indices 3..5 of that
buffer are deliberately degenerate. A green shader makes scope visible:

| Case | Expected 16x16 result |
|---|---|
| `native` | 256 red, without loading the DLL |
| `plain` | 256 red, DLL loaded, no overrides |
| `mesh` | 256 red, replacement mesh |
| `late` | 256 red; the later PS callback cannot affect the earlier mesh draw |
| `scoped` | 256 green; the replacement draw executes inside `CustomShader` |
| `scoped_off` | 256 red; the conditional scoped branch is disabled |

In `late`, `draw = from_caller` submits the parent parameters (first index 3)
with the currently bound replacement buffer, not a replay of its earlier
first-index-0 draw. It cannot turn that red draw green. The test explicitly
checks this result; it is **not** a failing test for a DX12-only regression.
Both APIs must agree. The D3D11 debug log and D3D12 draw notes must contain
exactly one replacement draw followed by one parent-range replay. Reversed,
missing, or repeated draws fail even when the final pixels remain red.
The ordered draws are also retained in `results.json`.
`scoped` verifies the shader itself works and there is
no extra parent-range redraw. DX12 draw notes and per-case logs are retained.

This is a narrow direct indexed-draw contract test. It does not cover
bundles, indirect or mesh draws, multi-queue recording, arbitrary state
overrides, capture hotkeys, config hot-reload, or any real game's rendering.

Run the draw-order checker tests without a DLL or graphics runtime:

```bat
python -m unittest discover -s tools/harness/shader-scope -p test_check.py
```
