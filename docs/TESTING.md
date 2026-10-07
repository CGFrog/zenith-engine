# Testing

How Zenith's tests are written, organised, and run. This page is the rule
book. `tools/tests/check_tests.py` enforces the parts a script can check: CI's
`docs` job and `tools/check.ps1` run it.

## The short version

- A unit test is a doctest `TEST_CASE` in its area's test binary, written with
  `CHECK`/`REQUIRE`, and it owns any files it writes through a
  `testsupport::TempDirectory`.
- Pick the tier by what the test needs: `unit` (CPU only, fast) is the
  default; `slow`, `vulkan`, and `benchmark` are for the rest.
- Run `ctest --preset unit` while you work, `ctest --preset cpu` before you
  push, and `ctest --preset gpu` when you touch graphics or windows.
- A bug fix starts with a test that fails without the fix.

## Tiers

Every test has exactly one tier label. `zenith_add_test` gives a test without
one the `unit` label.

| Tier | Label | For | Preset |
| --- | --- | --- | --- |
| Unit | `unit` | CPU-only, deterministic contracts; well under a second per case. Almost every test. | `unit` |
| Slow | `slow` | CPU work too slow for the everyday loop: configuring and building a generated project. | `cpu` (with `unit`) |
| GPU | `vulkan` (+ `smoke`, `validation`, or `capture`) | Behaviour that needs a Vulkan device: bounded engine or editor runs, the strict validation matrix, reference captures. | `gpu` |
| Benchmark | `benchmark` | Numbers to read. CTest only checks that it finishes. | `benchmark` |

Put a behaviour in the cheapest tier that can observe it. Logic that a GPU
test happens to exercise (culling, packet building, picking ids, editor
state) gets a unit test of its own. GPU tests are for what only a device can
show.

## Running tests

| Command | Runs |
| --- | --- |
| `ctest --preset unit` | The unit tier: the everyday loop (`tools/check.ps1` runs it). |
| `ctest --preset cpu` | Unit and slow: what CI runs on every OS. Run it before pushing. |
| `ctest --preset gpu` | The GPU tier. Needs a Vulkan device. |
| `ctest --preset benchmark` | The benchmarks. |
| `ctest --preset all` | Everything (`tools/check.ps1 -Full`). |
| `ctest --preset unit -R "engine.GameServices"` | One file's cases (`-R` is a regex over CTest names). |
| `zenith_engine_tests -tc="GameServices: timers"` | One case, run directly. `-ts=GameServices` runs a suite, `-ltc` lists the cases, and `-s` prints passing checks too. |

The presets use `build/` (Debug). With another build tree, pass the same
filters to `ctest --test-dir <tree>`, for example
`-L "^(unit|slow)$" -LE "^benchmark$"` for the cpu tier.

Without a GPU, Mesa's lavapipe runs the GPU tier on the CPU under a virtual X
server. Configure with `-DZENITH_TEST_TIMEOUT_SCALE=4`, then run
`xvfb-run -a ctest --preset gpu`. With the LunarG validation layer on
`VK_ADD_LAYER_PATH` and `ZENITH_VALIDATION_STRICT=1`, any validation message
fails the run. GPU tests exit 77, which CTest shows as *Skipped*, when no
device or layer is available.

## Writing a unit test

Unit tests live in `tests/<area>/<Thing>Tests.cpp`, where the area is
`engine`, `editor`, or `game`. Each area builds one binary,
`zenith_<area>_tests`, from the files listed in its `zenith_add_test_suite`
call in `tests/<area>/CMakeLists.txt`. Every case becomes its own CTest test,
named `<area>.<case name>`, and runs in its own process. A new case needs no
CMake change; a new file is added to that list.

```cpp
// What this file covers, in a sentence or two.

#include "engine/platform/VersionedJson.hpp"

#include "support/Doctest.hpp"

namespace {
const VersionedJsonFormat settings{.description = "test settings", .firstVersion = 2, .currentVersion = 4};
} // namespace

TEST_SUITE_BEGIN("VersionedJson");

TEST_CASE("VersionedJson: an older supported version is returned for the caller to migrate") {
    const testsupport::TempDirectory root("versioned-json-versions");
    testsupport::writeText(root / "outdated.json", R"({"version": 2, "value": 7})");

    const VersionedJsonResult outdated = readVersionedJson(root / "outdated.json", settings);

    REQUIRE(outdated.found);
    CHECK(outdated.version == 2);
    CHECK_MESSAGE(outdated.document.at("value") == 7, "the document is returned as written");
}

TEST_SUITE_END();
```

**Rules:**

- **Names.**
  - The file opens with `TEST_SUITE_BEGIN("<Thing>")`, matching the file name
    without `Tests`, and ends with `TEST_SUITE_END()`.
  - Every case is `"<Thing>: <behaviour>"`, where the behaviour is a phrase
    that reads as a sentence about what is true.
  - Case names are unique within the area and contain no comma or `* ? [ ]`,
    because CTest selects cases by name.
- **One behaviour per case.**
  - Group the checks of one behaviour in a case.
  - When several inputs share a setup, use `SUBCASE`s, or a loop with
    `CAPTURE(input)` or `INFO(...)` so a failure says which input failed.
- **Assertions.**
  - `REQUIRE` stops the case and `CHECK` carries on. Use `REQUIRE` when the rest
    of the case would be meaningless or crash without the condition, such as an
    optional that must hold a value, or a pointer that must not be null.
  - Write the comparison itself: `CHECK(a == b)` prints both values when it
    fails. `CHECK_MESSAGE(condition, why)` adds the reason.
  - doctest decomposes only one comparison, so a compound condition goes in
    parentheses: `CHECK((a && b))`. Prefer separate checks for each part.
  - Floats: `CHECK(x == doctest::Approx(0.25))`. glm values:
    `CHECK(testsupport::approxEqual(a, b))`, and `support/Doctest.hpp` prints
    glm values in failures.
  - Exceptions: `CHECK_THROWS_AS(expression, Type)`, `CHECK_NOTHROW(expression)`.
- **Files.**
  - Write only under a `testsupport::TempDirectory`. It is a fresh directory in
    the build tree's `test-temp/`, removed when it goes out of scope.
  - Read and write with `testsupport::readText`, `readBytes`, and `writeText`.
  - Never use `temp_directory_path()`, a fixed path, or the working directory
    (cases run from the top of the build tree).
  - Read assets through `ZENITH_ASSET_DIR` (`NEEDS_ASSETS` in the suite call).
- **Determinism.**
  - No randomness without a fixed seed, and no wall-clock assertions.
  - Never sleep a fixed time to wait for a worker: poll the observable result
    against a generous deadline (see the streaming check in
    `engine/AudioSystemTests.cpp`).
- **Independence.**
  - A case sets up everything it uses, through local values or a fixture
    struct.
  - It never relies on another case having run, on global state another case
    left, or on the order of cases. CTest runs each case in a separate process,
    so dependence on order shows up as a failure.
- **Fixtures.**
  - Editor tests build `ApplicationContext` and `EditorContext` from
    `editor/EditorTestFixture.hpp` and never from their own copy. When either
    context gains a required member, add it to the fixture.
  - Helpers used by more than one file go in `tests/support/`, not into a
    second copy.
- **Bug fixes.** Write the test first and watch it fail for the reason the bug
  report gives. Then fix the code and watch the test pass, and say so in the
  pull request.

## Standalone tests

Some tests are their own executable with their own `main()`:
- GPU smokes (`*SmokeTests.cpp`), which run a bounded frame loop;
- the strict validation matrix (`tests/validation/`);
- reference captures (`tests/capture/`);
- benchmarks (`*Benchmark*.cpp`);
- runs driven by command-line arguments (template module builds, the file
  watcher's native and polling modes).

They register with `zenith_add_test` and a tier label:
- A `vulkan` test gets `SKIP_RETURN_CODE 77` and `PROCESSORS 2` by default, so
  at most half of `ctest -j`'s slots run an engine on the shared GPU.
- It returns `testsupport::skipExitCode` (77) when it has no device.
- It is bounded with `ZENITH_SMOKE_FRAMES` and a `TIMEOUT` several times its
  loaded run time.

They use the same support helpers (`TempDirectory`, the file helpers, the glm
comparisons), and report failures with the frame and reason.

## Where things are

| Path | What |
| --- | --- |
| `tests/support/` | `zenith_test_support`: `Doctest.hpp` (include this), `TempDirectory`, `Files`, `Approx`, `Skip`; and `zenith_test_main`. |
| `tests/<area>/` | The area's unit tests, its standalone executables, and its `CMakeLists.txt`. |
| `cmake/ZenithTesting.cmake` | `zenith_add_test_suite` (unit binaries), `zenith_add_test` (standalone executables), the tier defaults, and `ZENITH_TEST_TIMEOUT_SCALE`. |
| `CMakePresets.json` | The `unit`, `cpu`, `gpu`, `benchmark`, and `all` test presets. |
| `tools/tests/check_tests.py` | The checks this page describes, and `legacy_tests.txt`, the files not converted yet. |

`tools/tests/legacy_tests.txt` lists files written before this scheme, with
their own `expect()` and `main()`. They are being converted to doctest
area by area. Convert a file when you change it substantially, and remove it
from the list in the same change. Never add a file to the list.
