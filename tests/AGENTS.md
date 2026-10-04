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
| `validation/` | Strict Vulkan validation smoke matrix (CMake only). [Validation](../docs/RENDERER_VALIDATION.md) |

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
Editor tests that need an `ApplicationContext` or `EditorContext` build it
from the shared fixture in `editor/EditorTestFixture.hpp` (derive from
`EditorTestFixture`; custom session actions derive from `TestSessionActions`
and are installed with `useActions`), never from their own copy. When either
context gains a required member, add it to the fixture.
