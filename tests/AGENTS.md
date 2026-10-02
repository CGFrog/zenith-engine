# Test workspace guide

Tests mirror the production dependency boundary:

| Path | Scope |
| --- | --- |
| `engine/` | Reusable subsystem contracts and scene/asset integration. |
| `editor/` | Authoring documents, history, settings, camera, and widgets. |
| `game/` | Demo application lifecycle and gameplay control integration. |
| `capture/` | Reference capture driver that renders manifest views and compares them with checked-in images. |
| `references/` | Capture manifests and reference PNGs; regenerate them as `docs/RENDERER_VALIDATION.md` describes. |

Prefer public behavior, deterministic inputs, and CPU-only execution when GPU
access is not intrinsic. Every test executable is listed explicitly in the root
`CMakeLists.txt`; moving or adding a test requires updating that list and its
CTest registration. Use temporary paths for files created by tests and clean up
only paths owned by the test.
