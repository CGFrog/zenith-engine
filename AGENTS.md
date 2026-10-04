# AGENTS.md

## Start here

- Read `docs/REPOSITORY_GUIDE.md`, the index of the workspace map, and
  `docs/ARCHITECTURE.md` before changing subsystem boundaries. Per-subsystem
  docs live under `docs/engine/`, `docs/editor/`, and `docs/game/`; read the
  one for the subsystem you change.
- Feature details belong in the subsystem doc. Change a directory-map row only
  when a folder is added, removed or changes purpose.
- Read the nearest nested `AGENTS.md` before editing `src/engine/`,
  `src/editor/`, `src/game/`, or `tests/`; those files add local guidance to
  this root policy.
- Treat `build/` as generated output. Search checked-in code with
  `rg --files src tests` and update the explicit source lists in
  `CMakeLists.txt` whenever files move.
- Use include paths rooted at the published `src/` include directory when a
  dependency crosses folders, for example `engine/scene/Scene.hpp`.

## Project intent

Zenith is a C++20 game engine intended to support many independent games over time. Optimize for code that is easy to read, test, replace, and reuse. A feature is not complete if it works only for the current demo or introduces unnecessary coupling between subsystems.

## Priorities

When tradeoffs are necessary, prefer this order:

1. Correctness and explicit ownership/lifetime.
2. Clear separation of concerns and one-way dependencies.
3. Readability and maintainability.
4. Reusable, project-agnostic engine APIs.
5. Testability and deterministic behavior.
6. Performance supported by measurement.
7. Concision or cleverness.

Choose the simplest design that preserves clean boundaries. Do not add abstractions solely for hypothetical use, but do not place game-specific shortcuts in reusable engine code.

## Architecture boundaries

- `src/engine/` is reusable engine code. It must not depend on a particular game, level, asset, control scheme, or executable.
- `src/main.cpp` is the composition root. Keep startup, project selection, and top-level application wiring here or in a future application/game layer.
- `src/game/` contains the current demo and gameplay composition. Treat it as a client of engine services; do not make lower-level systems depend on it.
- `assets/` and `shaders/` are runtime content. Engine systems may provide generic loading and rendering facilities, but must not hard-code knowledge of specific project assets.
- `tests/` should exercise public behavior and important subsystem contracts. Prefer CPU-only tests where GPU access is not intrinsic to the behavior.

Preserve this dependency direction:

```text
application / game composition
            |
            v
        engine API
            |
            v
focused subsystems (scene, assets, input, physics, graphics, window)
            |
            v
platform and third-party libraries
```

Dependencies must not point upward. Keep graphics independent of gameplay rules, physics independent of rendering, input independent of a specific controller, and asset parsing independent of GPU resource ownership. Communicate across boundaries through small domain types, handles, configuration, results, or narrow interfaces rather than reaching into another subsystem's internals.

## Design guidelines

- Give each class, function, and translation unit one clear responsibility.
- Prefer cohesive modules and composition over large manager classes, inheritance trees, global state, or service locators.
- Keep public interfaces small. Hide Vulkan, SDL, Jolt, cgltf, and other implementation details unless deliberate native access is required.
- Make ownership visible with values, RAII, references, and smart pointers as appropriate. Avoid ambiguous raw owning pointers and manual cleanup paths.
- Pass dependencies explicitly. Avoid hidden initialization order and mutable global state.
- Separate policy from mechanism: engine code supplies reusable mechanisms; the game/application chooses content and behavior.
- Use configuration structures for meaningful variability instead of scattered constants or project-specific branches.
- Prefer opaque/generational handles at subsystem boundaries when they protect ownership and lifetime, as the asset system already does.
- Return descriptive errors at recoverable boundaries. Do not silently ignore invalid state.
- Keep functions short enough to understand locally. Extract named operations when a function mixes levels of abstraction or multiple responsibilities.
- Avoid premature generalization. Introduce an interface or abstraction when there is a real boundary, a testing need, or more than one meaningful implementation.
- Document invariants, ownership, units, coordinate conventions, and non-obvious synchronization—not syntax already evident from the code.

## Readability and C++ style

- Follow `.clang-format`; do not hand-format against it.
- Use descriptive names. Avoid unexplained abbreviations, boolean parameters with unclear call-site meaning, and generic names such as `data`, `manager`, or `util` when a domain name is available.
- Keep headers focused and minimize transitive includes. Forward-declare when it genuinely reduces coupling without obscuring the code.
- Prefer scoped enums, `const`, standard library types, and strong domain types where they prevent invalid combinations.
- Keep related validation and state transitions together. Make the happy path easy to follow and handle failures near the operation that can fail.
- Do not mix unrelated cleanup, refactoring, or formatting into a focused change, but callout any needed changes when possible.
- Match existing conventions unless intentionally improving them consistently across the affected module.
- Always use brackets in if statements.

## Adding or changing features

Before implementing a feature:

1. Identify whether it belongs to the engine, a subsystem, or game/application composition.
2. Define the narrow contract between the participating modules.
3. Keep project-specific decisions at the highest possible layer.
4. Add or update tests for observable behavior and failure cases.

When a change crosses several subsystems, integrate them at the composition layer rather than creating circular knowledge. If a file is accumulating unrelated responsibilities, split it by domain responsibility rather than by arbitrary size.

Maintain backward compatibility for reusable public APIs when practical. When an API must change, update every caller and relevant test in the same change; do not leave compatibility shims without a clear need.

## Build and verification

Configure and build from the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel 4
```

Run the automated tests (in parallel; GPU tests count double against `-j`, so
at most half the slots launch the engine at once):

```powershell
ctest --test-dir build -C Debug --output-on-failure -j 8
```

For a quick CPU-only loop while iterating, skip the Vulkan and benchmark tests:

```powershell
ctest --test-dir build -C Debug --output-on-failure -j 8 -LE "vulkan|benchmark"
```

Format C++ sources after editing them:

```powershell
clang-format -i --style=file (rg --files src tests -g '*.cpp' -g '*.hpp')
```

For graphics or windowing changes, also run an appropriate smoke test when the environment supports Vulkan. Use `ZENITH_SMOKE_FRAMES` for bounded automated runs. If a verification step cannot be run, state that clearly when handing off the change.

## Completion standard

A change is ready when it:

- has an obvious home and respects dependency direction;
- keeps engine code free of current-project assumptions;
- exposes the smallest useful API;
- handles ownership, errors, and lifecycle explicitly;
- includes proportional tests or verification;
- is formatted and builds without introducing warnings; and
- leaves the code easier—or at least no harder—to understand and reuse.
