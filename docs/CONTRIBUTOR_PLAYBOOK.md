# Contributor playbook

Checklists for the change types that come up most often in the engine and
editor. Each lists every file you must touch, so a change is complete the
first time. They condense rules from [`AGENTS.md`](../AGENTS.md) and the
nested `src/*/AGENTS.md` and `tests/AGENTS.md` files; those remain the
authority. To find the code involved, use the [code map](CODE_MAP.md).

**Always:** a new header opens with a one-sentence summary comment after
`#pragma once` (a `.cpp` without a header, at its top); new source files go in the explicit source list of the nearest
`CMakeLists.txt`; includes that cross folders are rooted at `src/`
(`engine/scene/Scene.hpp`); every `if` has braces; run `tools/check.ps1`
(build + CPU tests + clang-format of changed files) before handing off; update
the subsystem doc under `docs/engine/` or `docs/editor/` that describes the
behaviour you changed.

## Decide where a change belongs

| If the change is... | It goes in | Never |
| --- | --- | --- |
| Reusable runtime mechanism (any game could use it) | `src/engine/<subsystem>/` | Name a game, asset, or control scheme. |
| Authoring workflow, ImGui, document policy | `src/editor/<folder>/` | Be depended on by engine or game code. |
| The demo's content, keys, menus | `src/game/` | Be depended on by the engine. |
| A specific game's rules | That game's module (`templates/`, a project's `src/`) | Go into the engine. |
| Runtime content | `assets/`, `shaders/` | Be hard-coded in engine code. |

Dependencies point down only: game/editor → engine API → subsystems → platform
and third-party libraries.

## Add a serialized engine component type

1. Declare the struct in the subsystem that owns its meaning (e.g.
   `src/engine/physics/PhysicsComponents.hpp`, `src/engine/scene/Scene.hpp`
   for core rendering components).
2. Build its `ComponentType` in the `scene/` family file for its domain:
   `RenderComponentTypes.cpp`, `LightComponentTypes.cpp`,
   `PhysicsComponentTypes.cpp`, `AnimationComponentTypes.cpp`,
   `AudioComponentTypes.cpp`, `GameplayComponentTypes.cpp`,
   `NavigationComponentTypes.cpp`, `CollectionComponentTypes.cpp`, or
   `PrefabComponentTypes.cpp`. Use `ComponentTypeBuilder<T>(key, displayName)`
   with `.category(...)` and `.fields(Fields<T>()...)`.
3. **Append** it in `scene/BuiltInComponentTypes.cpp` (the order is fixed;
   never reorder).
4. The scene document, Inspector, undo, prefab overrides, and the Add menu
   discover it from the registry; no editor change is needed unless reflected
   fields cannot express the UI.
5. If a system consumes it, wire that system in its subsystem (see below).
6. Test: serialization round trip and behaviour under `tests/engine/`.
7. If the type is visible to game modules and changes a struct they share
   (`SystemContext`, `ModuleRegistrar`, a registered descriptor, a component
   layout modules read), bump `ZENITH_MODULE_API_VERSION` in
   `src/engine/module/ModuleApi.hpp` and record why in
   [Game modules](engine/modules.md).
8. Doc: the subsystem page's component table.

## Change an existing component's fields

1. Never change a component key or an existing field key's meaning silently.
2. `.schemaVersion(n + 1)` and `.migration(n, [](nlohmann::json& json) { ...;
   return SceneDocumentResult{.success = true}; })` on the builder.
3. Test that a version-*n* document loads (scene migration tests live beside
   the family's tests in `tests/engine/`).
4. Check `projects/` scenes and `templates/*/scenes` still load
   (`zenith_project_runtime_tests` verifies every project scene; the asset
   reference workflow tests load every checked-in scene).

## Add an engine system that runs every frame

- **Built-in gameplay system** (runs in every host before game systems):
  add it in `src/engine/module/BuiltInSystems.*` with a `zenith.` name, a
  phase, and `RunIn` flags. Document it in [Game modules](engine/modules.md).
- **Engine-internal step** (physics, animation, audio sync): call it from
  `Engine::run` / the application scene hooks in the right frame stage
  ([Architecture › Frame sequence](ARCHITECTURE.md#frame-sequence)).
- Add the files to `zenith_engine` in `src/engine/CMakeLists.txt`.
- Test with a CPU-only scene in `tests/engine/`.

## Add an editor operator (a command)

1. Write the `Operator` (`id` like `object.do_thing`, `label`,
   `description`, `flags`, `poll`, `execute`) in the family file it belongs
   to under `src/editor/operators/` (document-only operators must run
   headless) or, if it needs session state, behind the narrow
   `application/EditorSessionCommands` interfaces.
2. A brand-new family is **one line** in
   `operators/EditorOperatorFamilies.cpp`; never register from
   `EditorSession`.
3. Default key: `operators/DefaultKeymap.cpp`.
4. Menu entry: `menus/DefaultMenus.cpp` (registered menus) or
   `application/EditorMainMenu.cpp` (main menu bar), naming the operator id.
5. `Undo` operators must not write history themselves; give them a `scope`.
   Operators that create objects take a `location` filled by
   `invokeDefaults` from the 3D cursor.
6. Document-only operators poll `editMode()` so Play refuses them with a
   message.
7. Test headless under `tests/editor/` (see the operator tests there and
   `EditorTestFixture.hpp`).
8. Doc: [Operators and keymaps](editor/operators.md).

## Add an editor panel / area type

1. An `EditorPanel` subclass in `src/editor/panels/`; it receives everything
   through `EditorContext` and edits the scene only through `SceneEditing`.
2. Register its area type in `builtInAreaTypes` (never rename a type id);
   see [Areas and workspaces](editor/areas-workspaces.md#making-a-panel-visible-as-an-area-type).
3. Colours from `theme/EditorPalette`, sizes from `editorArtPixel`, controls
   from `theme/EditorWidgets`.
4. Add it to `src/editor/CMakeLists.txt`; test it headless (area types are
   drawn headless in `tests/editor/`).

## Add a field to the Inspector with a custom widget

Prefer presentation hints on `Fields<T>` (`.drag`, `.slider`, `.angle`,
`.color`, `.asset`, `.options`, `.nameList`, `.tooltip`, `.format`). Only when
reflection cannot express the UI, add a custom editor in
`src/editor/interaction/EntityPropertiesView` (it must edit only the
component it is given; the view propagates to multi-selection).

## Add a render pass or shader

1. GLSL under `shaders/` with a stage extension (`.vert`, `.frag`, `.comp`);
   the build compiles it to `build/shaders/*.spv` automatically. Resolve it
   at runtime with `shaderRoot()` (`engine/resources/ResourceLocator.hpp`).
2. The pass under `src/engine/graphics/passes/` (or `graphics/` for frame
   infrastructure), added to `src/engine/CMakeLists.txt`.
3. Record it from the renderer at the right point of the
   [render flow](engine/graphics.md#render-flow). If it is a per-frame decision,
   it comes from `MeshPassFramePlan`; mirror it in
   `Renderer::updateRenderGraphPlan` / `RenderGraphShape` so the render-graph
   model validates ordering and hazards.
4. Give it a GPU timestamp scope so it appears in Render Statistics and
   benchmarks.
5. Settings that toggle it belong in `RenderSettings`
   (`engine/graphics/Rendering.hpp`), persisted by the project and exposed in
   the editor's Graphics panel / Properties Render tab.
6. Verify: a bounded Vulkan smoke (`ZENITH_SMOKE_FRAMES`), the reference
   captures if output changes ([Renderer validation](RENDERER_VALIDATION.md)),
   and a CPU test for any pure logic (cluster math, plan decisions).
7. Docs: [Graphics passes](engine/graphics-passes.md) or
   [post-processing](engine/graphics-post.md), and [Shaders](engine/shaders.md).

## Add a gameplay service to `GameServices`

1. Implement the mechanism in its subsystem (no game policy).
2. Expose it on the group whose question it answers
   (`src/engine/module/GameServiceGroups.*`): what changes the scene
   (`WorldServices`, `world()`), when things happen (`TimeServices`,
   `time()`), the session's own data (`ContentServices`, `content()`), or
   what lies outside the scene (`OutsideServices`, `outside()`). Only what
   every group's users share (events, logging, the host's lifecycle calls)
   goes on `GameServices` itself. It must be destroyed before the module that
   used it unloads, and after anything in a later-declared group that refers
   to it.
3. This changes `GameServices`' layout: bump `ZENITH_MODULE_API_VERSION`.
4. Test in `tests/engine/GameServicesTests.cpp`.
5. Docs: [Game modules › GameServices](engine/modules.md#gameservices) and
   the user guide's [Gameplay services](guide/06-gameplay-services.md).

## Add a test

[Testing](TESTING.md) has the rules; this is the checklist.

1. **Unit test** (most tests):
   - Add doctest cases to `tests/<area>/<Thing>Tests.cpp`. It opens with
     `TEST_SUITE_BEGIN("<Thing>")`, its cases are named `"<Thing>: <behaviour>"`,
     and it includes `support/Doctest.hpp`. See
     `tests/engine/VersionedJsonTests.cpp`.
   - A new file goes in the `SOURCES` of its area's `zenith_add_test_suite` in
     `tests/<area>/CMakeLists.txt`.
   - Files go under a `testsupport::TempDirectory`.
2. **Standalone test** (GPU smoke, benchmark, argument-driven run): its own
   executable, registered with
   `zenith_add_test(<ctest_name> SOURCES <File>.cpp LIBS ... LABELS <tier>)`.
   A `vulkan` test returns 77 without a device.
3. Editor tests build contexts from `tests/editor/EditorTestFixture.hpp`.
4. A bug fix: the test fails before the fix.
5. Run it: `ctest --preset unit -R "<area>.<Thing>"`, or
   `zenith_<area>_tests -tc="<Thing>: *"`. Then `python tools/tests/check_tests.py`.

## Add a project template

1. A folder under `templates/` with `template.json` (name, description, and
   what the manifest records), its `CMakeLists.txt`, `src/`, `assets/`,
   `scenes/`. `{{UPPER_CASE}}` tokens are substituted at generation.
2. The build copies `templates/` to `build/templates/`
   (`zenith_editor_templates`).
3. Test generation and, for C++ templates, a module build (see the
   `zenith_project_template_module_build` and
   `zenith_fps_template_module_build` tests in `tests/editor/CMakeLists.txt`).

## Move or rename a file

1. `git mv` it.
2. Update the explicit source list in the nearest `CMakeLists.txt`.
3. Update every include (`git grep -n '<old/include/path>'`).
4. Run `python tools/docs/code_index.py` to refresh the source index in
   [CODE_MAP.md](CODE_MAP.md), and update any doc naming the path.

## Before handing off

- [ ] `tools/check.ps1` (or `cmake --build --preset debug`, `python tools/tests/check_tests.py`, `ctest --preset unit`, clang-format on changed files) passes with no new warnings; `ctest --preset cpu` before pushing.
- [ ] Graphics/window changes: a bounded smoke run where Vulkan is available, or state that it could not be run.
- [ ] Engine code has no game/editor knowledge; no upward includes.
- [ ] Subsystem doc updated; if a file was added, moved, or removed, `python tools/docs/code_index.py` run (and a *Find it fast* row added for a new capability).
- [ ] `python tools/docs/check_links.py` passes (CI's `docs` job runs it with both generators' `--check`).
- [ ] User-visible behaviour changed? Update the [user guide](guide/README.md) page and add a `📷 Screenshot needed` callout if the UI changed (then run `tools/docs/screenshot_checklist.py`).
