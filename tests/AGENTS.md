# Test workspace guide

Tests mirror the production dependency boundary:

| Path | Scope |
| --- | --- |
| `engine/` | Reusable subsystem contracts and scene/asset integration. [Engine docs](../docs/engine/) |
| `editor/` | Authoring documents, history, settings, camera, and widgets. [Editor docs](../docs/editor/) |
| `game/` | Demo application lifecycle, gameplay control integration, and the game's project runtime. [Demo](../docs/game/demo.md) |
| `packaging/` | CMake script tests (`cmake -P`) of the packaging configure checks in `cmake/`. [Releasing](../docs/RELEASING.md) |
| `capture/` | Reference capture driver that renders manifest views and compares them with references. [Validation](../docs/RENDERER_VALIDATION.md) |
| `references/` | Capture manifests and reference PNGs, regenerated as validation describes. [Validation](../docs/RENDERER_VALIDATION.md) |

Prefer public behavior, deterministic inputs, and CPU-only execution when GPU
access is not intrinsic. Tests run in parallel (`ctest -j 8`), so each test
owns its temporary paths, and tests that launch the engine set `PROCESSORS 2`.
CTest points `TMP`/`TEMP`/`TMPDIR` at the build tree's `test-temp/`
(the `zenith_test_temp` fixture, which every test requires), so suites in other
build trees never share temporary paths with this one; a test registered after
that block in `CMakeLists.txt` would miss it. Never wait for a worker thread with
a fixed sleep: poll the observable result against a generous deadline (see the
streaming check in `engine/AudioSystemTests.cpp`). Give engine-launching tests
timeouts several times their loaded run time.
Reference captures register one CTest per view from `captures.json`. Every test executable is listed explicitly in the root
`CMakeLists.txt`; moving or adding a test requires updating that list and its
CTest registration. Use temporary paths for files created by tests and clean up
only paths owned by the test.
