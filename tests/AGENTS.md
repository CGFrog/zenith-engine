# Test workspace guide

Tests mirror the production dependency boundary:

| Path | Scope |
| --- | --- |
| `support/` | Shared test helpers (`zenith_test_support`) and the doctest main. [Testing](../docs/TESTING.md) |
| `engine/` | Reusable subsystem contracts and scene/asset integration. [Engine docs](../docs/engine/) |
| `editor/` | Authoring documents, history, settings, camera, and widgets. [Editor docs](../docs/editor/) |
| `game/` | Demo application lifecycle, gameplay control integration, and the game's project runtime. [Demo](../docs/game/demo.md) |
| `packaging/` | CMake script tests (`cmake -P`) of the packaging configure checks in `cmake/`. [Releasing](../docs/RELEASING.md) |
| `capture/` | Reference capture driver that renders manifest views and compares them with references. [Validation](../docs/RENDERER_VALIDATION.md) |
| `references/` | Capture manifests and reference PNGs, regenerated as validation describes. [Validation](../docs/RENDERER_VALIDATION.md) |
| `validation/` | Strict Vulkan validation smoke matrix (CMake only). [Validation](../docs/RENDERER_VALIDATION.md) |

[`docs/TESTING.md`](../docs/TESTING.md) is the rule book for tests; read it
before adding or changing one. In short:

- Unit tests are doctest cases (`TEST_CASE("<Thing>: <behaviour>")` after
  `TEST_SUITE_BEGIN("<Thing>")`, including `support/Doctest.hpp`) in
  `<area>/<Thing>Tests.cpp`, listed in the area's `zenith_add_test_suite`.
  Each case is the CTest `<area>.<case name>` and runs in its own process.
- Standalone executables (GPU smokes, benchmarks, captures, argument-driven
  runs) register with `zenith_add_test` and a tier label (`unit`, `slow`,
  `vulkan`, `benchmark`); a `vulkan` test defaults to `SKIP_RETURN_CODE 77`
  and `PROCESSORS 2` and returns 77 without a device.
- Shared helpers live in `support/` (`TempDirectory`, `readText`/`writeText`,
  `approxEqual`, `skipExitCode`); never add a second copy of a helper to a
  test file. Files a test writes go under its own `testsupport::TempDirectory`.
- Never wait for a worker thread with a fixed sleep: poll the observable
  result against a generous deadline (see the streaming check in
  `engine/AudioSystemTests.cpp`). Give engine-launching tests timeouts several
  times their loaded run time.
- `python tools/tests/check_tests.py` enforces these rules; files listed in
  `tools/tests/legacy_tests.txt` predate them and are converted area by area.
  Remove a file from that list in the change that converts it, and never add one.
- CTest points `TMP`/`TEMP`/`TMPDIR` at the build tree's `test-temp/`
  (the `zenith_test_temp` fixture), so suites in other build trees never share
  temporary paths with this one. A folder that registers tests only with plain
  `add_test` must call `zenith_use_test_temp_directory()`.
- Reference captures register one CTest per view from `captures.json`.

Editor tests that need an `ApplicationContext` or `EditorContext` build it
from the shared fixture in `editor/EditorTestFixture.hpp` (derive from
`EditorTestFixture`; custom session actions derive from `TestSessionActions`
and are installed with `useActions`), never from their own copy. When either
context gains a required member, add it to the fixture.
