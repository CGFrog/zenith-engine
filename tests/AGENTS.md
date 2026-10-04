# Test workspace guide

Tests mirror the production dependency boundary:

| Path | Scope |
| --- | --- |
| `engine/` | Reusable subsystem contracts and scene/asset integration. |
| `editor/` | Authoring documents, history, settings, camera, and widgets. |
| `game/` | Demo application lifecycle, gameplay control integration, and the game's project runtime. |
| `packaging/` | CMake script tests (`cmake -P`) of the packaging configure checks in `cmake/`. |
| `capture/` | Reference capture driver that renders manifest views and compares them with checked-in images. |
| `references/` | Capture manifests and reference PNGs; regenerate them as `docs/RENDERER_VALIDATION.md` describes. |
| `validation/` | The strict Vulkan validation smoke matrix (CMake only: it runs the game, the editor, and smoke executables from `engine/`). |

Prefer public behavior, deterministic inputs, and CPU-only execution when GPU
access is not intrinsic. Tests run in parallel (`ctest -j 8`), so each test
owns its temporary paths, and tests that launch the engine set `PROCESSORS 2`.
CTest points `TMP`/`TEMP`/`TMPDIR` at the build tree's `test-temp/`
(the `zenith_test_temp` fixture, which every test requires), so suites in other
build trees never share temporary paths with this one. `zenith_add_test`
attaches it to every test of its directory; a folder that registers tests only
with plain `add_test` must call `zenith_use_test_temp_directory()`. Never wait for a worker thread with
a fixed sleep: poll the observable result against a generous deadline (see the
streaming check in `engine/AudioSystemTests.cpp`). Give engine-launching tests
timeouts several times their loaded run time.
Reference captures register one CTest per view from `captures.json`. Each folder's `CMakeLists.txt` declares its tests
explicitly with `zenith_add_test` (`cmake/ZenithTesting.cmake`), which builds
the executable with the project warnings and registers the CTest; moving or
adding a test requires updating the nearest `CMakeLists.txt`. Use temporary paths for files created by tests and clean up
only paths owned by the test.
