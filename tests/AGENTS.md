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

Prefer public behavior, deterministic inputs, and CPU-only execution when GPU
access is not intrinsic. Tests run in parallel (`ctest -j 8`), so each test
owns its temporary paths, and tests that launch the engine set `PROCESSORS 2`.
Reference captures register one CTest per view from `captures.json`. Every test executable is listed explicitly in the root
`CMakeLists.txt`; moving or adding a test requires updating that list and its
CTest registration. Use temporary paths for files created by tests and clean up
only paths owned by the test.
