# 6. Test, profile, and package

## Headless gameplay tests

Put game tests under `tests/fps/` and link them to `zenith_fps`. Keep logic
systems callable without a window or GPU. Useful first contracts are:

- diagonal movement is normalized;
- pitch never exceeds ±89 degrees;
- an airborne player has reduced control;
- jumping requires a grounded contact and a pressed edge;
- a shot consumes exactly one round;
- cooldown prevents an early second shot;
- damage applies only to the raycast hit entity; and
- requesting restart unloads and reconstructs the scene cleanly.

Register the executable explicitly:

```cmake
add_executable(zenith_fps_tests tests/fps/FpsGameplayTests.cpp)
target_link_libraries(zenith_fps_tests PRIVATE zenith_fps)
add_test(NAME zenith_fps_gameplay COMMAND zenith_fps_tests)
```

For physics-dependent tests, initialize `PhysicsWorld` with deterministic
capacities and disabled audio output, following
`tests/game/ApplicationLifecycleTests.cpp` and `tests/engine/PhysicsTests.cpp`.

## Manual acceptance pass

Verify this checklist in a Debug build:

1. The authored scene loads from a clean build directory.
2. Missing scene or model files return a useful error without a crash.
3. WASD, sprint, jump, mouse look, and Escape capture behave correctly.
4. The player cannot jump in mid-air or rotate the capsule body.
5. Shots originate at the camera, stop at the nearest collider, and consume ammunition.
6. Destroyed targets release their physics bodies.
7. Restart can be repeated without accumulating entities, voices, or bodies.
8. Resize, minimize, and restore do not corrupt the viewport.

## Bounded Vulkan smoke run

```powershell
$env:ZENITH_SMOKE_FRAMES='120'
build\Debug\zenith_fps_game.exe
Remove-Item Env:ZENITH_SMOKE_FRAMES
```

The process should present 120 frames and exit successfully. Run CTest as well:

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

## Profile before optimizing

Use a Release build and the built-in fixed workload:

```powershell
cmake --build build --config Release --parallel 4
$env:ZENITH_BENCHMARK_FRAMES='2000'
build\Release\zenith_fps_game.exe
```

The benchmark prints median and p95 CPU/GPU timing as JSON. Compare changes on
the same machine, driver, scene, and present mode. The engine owns
`ZENITH_BENCHMARK_FRAMES`; the existing demo's `ZENITH_BENCHMARK_OBJECTS`
population is game-side policy. If the FPS needs a scalable workload, reproduce
that deterministic grid helper in `TrainingScene` or author a fixed benchmark
scene. Follow
[Renderer validation](../../RENDERER_VALIDATION.md) for the project thresholds.

## Runtime files

The executable needs its copied assets and compiled shaders. The project-level
CPack rules assemble these automatically:

```text
archive root/
|- zenith_fps_game.exe
|- assets/     models, scenes, textures, and audio
|- shaders/    compiled .spv files
|- licenses/
`- content-manifest.json
```

Resolve content with the engine `ResourceLocator`, never a source/build path.
See [Release packaging](../../RELEASING.md) for archive generation and the
clean-extraction verification command.

A game that does not need its own executable can instead register its
components and systems in a game module (a C++ project from the editor's
Project Hub) and ship with the stock `zenith` runtime: packaging the project
installs its module, assets, and every scene, and the packaged `zenith` loads
the module and runs its Play systems with the player's input (Release
packaging, "The packaged runtime").

## Where to go next

Good game-layer extensions are enemy state machines, pickups, score rules,
weapon switching, and data-driven tuning. Engine work should be justified by a
reusable need—examples include skeletal animation, alpha materials, navigation,
input action mapping, and relocatable runtime content.
