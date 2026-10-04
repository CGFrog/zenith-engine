# Repository guide

This guide is the index of Zenith's checked-in files. It explains what each element
owns, what depends on it, and where a change belongs. The top-level layout,
build, runtime content, and tests are described here; each subsystem has its
own document under `docs/engine/`, `docs/editor/`, and `docs/game/` (see
[Subsystem docs](#subsystem-docs)). For the runtime sequence
and subsystem contracts, see [Architecture](ARCHITECTURE.md). For commands and
common changes, see [Development](DEVELOPMENT.md). The remaining optimization
objectives and their measurement gates are in
[Performance roadmap](PERFORMANCE_ROADMAP.md). How a game module extends the
editor is in [Editor extensions](EDITOR_EXTENSIONS.md). The phased plan and
status for end-to-end gameplay programming (engine gameplay services,
Behaviours, the editor script workflow) is in
[Gameplay roadmap](GAMEPLAY_ROADMAP.md).

## Top level

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Declares the C++20 project, includes `cmake/Dependencies.cmake` (pinned third-party dependencies), adds `src/` and `tests/`, compiles shaders, copies and cooks assets, and installs and packages releases. Each source folder's own `CMakeLists.txt` lists its sources. |
| `README.md` | Quick-start overview, controls, current feature summary, and links into these detailed guides. |
| `AGENTS.md` | Root contributor policy and navigation. Scoped guides under `src/engine/`, `src/editor/`, `src/game/`, and `tests/` add local ownership rules. |
| `.clang-format` | Formatting rules for C++ headers and sources. |
| `.gitignore` | Excludes generated and local files such as the build tree. |
| `.vscode/` | Optional editor configuration. It is not required by the build. |
| `assets/` | Source runtime content copied into the build tree. |
| `shaders/` | GLSL source compiled to SPIR-V during the build. |
| `src/` | Application composition and reusable engine implementation. |
| `tests/` | CPU-side subsystem and integration tests registered with CTest. |
| `tools/showcase/` | Blender and Python scripts that regenerate the renderer showcase content. |
| `tools/irradiance_room/` | Standard-library Python script that regenerates the irradiance-volume test interior. |
| `tools/color_grading/` | Standard-library Python script that regenerates the example `.cube` colour grade. |
| `projects/` | Project manifests and scene documents (`demo`, the renderer `showcase`, and the `irradiance_room` interior). |
| `templates/` | Project templates the editor's Project Hub generates projects from, each described by its `template.json`: `cpp_game/` is a C++ game project (CMake game module, example component and system, and the code generators' `snippets/`), and `fps_game/` a playable first-person course. `{{UPPER_CASE}}` tokens are substituted. The build copies them to `build/templates/`. |
| `cmake/` | `Dependencies.cmake` (FetchContent pins and wrapper targets), `CompilerSettings.cmake` (`zenith_target_warnings`), `ZenithTesting.cmake` (`zenith_add_test`), packaging scripts, and the inputs of the files configuration writes for out-of-tree game modules (`ZenithEngineConfig.cmake`, `ZenithEngineTargets-<config>.cmake`, `zenith-engine.json`). |
| `build/` | Generated configuration, dependencies, binaries, copied assets, and compiled shaders. Do not edit generated contents. |

## Workspace layout

```text
src/
|- engine/                 reusable runtime library
|  |- application/         lifecycle contracts
|  |- assets/              CPU asset import and storage
|  |- audio/               decoding, streaming, and mixing
|  |- graphics/            rendering and Vulkan implementation
|  |- input/ jobs/ physics/ resources/ scene/ ui/ window/
|  |- module/              game module loading and system scheduling
|  `- project/             shared project manifests and scene paths
|- editor/                 authoring application
|  |- application/ document/ interaction/ modules/ operators/ panels/ project/ settings/ theme/
|- game/                   current demo client
|  |- application/ scene/  game executable composition (command line, runtime
|  |                       gameplay, ProjectApplication, the demo's application)
|  |- gameplay/ input/ player/  gameplay registered by the demo game module
|  |- module/              zenith_game_demo entry points
`- main.cpp                game executable composition root
tests/
|- engine/                 reusable subsystem contracts
|  `- modules/             game module libraries loaded by the module tests
|- editor/                 authoring behavior
|  `- modules/             game modules with editor extensions for the module host tests
|- game/                   demo integration and the game's project runtime
|- capture/                reference capture driver
|- packaging/              CMake script tests of the packaging configure checks
`- validation/             strict Vulkan validation smoke matrix (CMake only)
```

Long-form project tutorials live under `docs/tutorials/`. The
[first-person game tutorial](tutorials/fps-game/README.md) makes a game from
the First-Person Shooter template in the editor, with game rules in the
project's module and the engine's mechanisms beneath them.

Folder-crossing includes use paths rooted at `src/`. CMake source lists are
explicit, so a file move must update the nearest `CMakeLists.txt` as well as
include sites.

## Build description

The root `CMakeLists.txt`, `src/CMakeLists.txt`, and `src/{engine,game,editor}/CMakeLists.txt`
create these targets (`tests/*/CMakeLists.txt` add the test executables):

| Target | Kind | Responsibility |
| --- | --- | --- |
| `zenith_engine` | Shared library | Reusable application lifecycle, scene, asset, audio, input, physics, graphics, window, and game module systems. Exports every function (`WINDOWS_EXPORT_ALL_SYMBOLS`) and Dear ImGui plus ImGuizmo (`IMGUI_API`); links volk, Jolt, shaderc, meshoptimizer, bc7enc, and Recast/Detour privately. |
| `zenith_demo_gameplay` | Static library (PIC) | Demo gameplay registered by `registerDemoGameplay`: item collection components and system, and the demo input bindings. |
| `zenith_game_demo` | Module | The demo game module (`ZENITH_GAME_MODULE("demo", ...)`) the editor and the game load for the demo project. Each build copies it to the git-ignored `projects/modules/zenith_game_demo.zmodule`, the path `projects/demo.zenith-project` names; `install` places it at `modules/` beside the packaged project. |
| `zenith_demo` | Static library | Game executable composition: command line (`GameLaunch`), package verification, the gameplay to play (`RuntimeGameplay`: a project's module, or the demo gameplay linked in for runs without one), the project-agnostic `ProjectApplication`, and the demo's application, menus, settings, audio, and scenes. |
| `zenith` | Executable | Thin composition root: parses `[--verify-package] [--project <file>] [--scene <file>]`, loads the project's game module, initializes `Engine`, and runs `DemoApplication` or `ProjectApplication`. |
| `zenith_editor_app` | Static library | Editor lifecycle, documents, history, interaction, and settings. |
| `zenith_editor` | Executable | Thin editor composition root. Knows the source `templates/`, its build directory, and its configuration, for the Project Hub. |
| `zenith_editor_templates` | Custom target | Copies `templates/` to `build/templates/` for the Project Hub. |
| `zenith_shaders` | Custom target | Recursively finds recognized GLSL stages in `shaders/` and compiles each file with glslang for Vulkan 1.4. |
| `zenith_assets` | Custom target | Copies `assets/` to `build/assets/` so runtime paths do not depend on the process working directory. |
| `zenith_cooked_textures` | Custom target | Runs `zenith_asset_build cook-model-textures` on every `assets/*.gltf`, writing BC7/BC5 KTX2 entries to `build/derived/`. |
| `zenith_bc7enc` | Static library | The bc7enc_rdo BC7/BC5 block encoders and decoders, linked privately into the engine for texture cooking. |
| `zenith_recast` | Static library | The Recast and Detour sources, linked privately into the engine for navigation. |
| `zenith_asset_tests` | Test executable | Asset importing and scene hierarchy behavior. |
| `zenith_game_module_tests` | Test executable | Loads, reloads, and unloads the `zenith_test_*_module` libraries: registration, system order and modes, shadow copies, and refused modules. |
| `zenith_game_module_host_tests` | Test executable | Hosts `zenith_editor_test_module` in the editor's `GameModuleHost` without a window: module components through the document, edit/play systems, editor operators, panels, sidebar tabs, inspector overrides (multi-entity edit and undo), and menus, and their release on unload, hot reload over the `_v2` build, and a failed reload that keeps module data as JSON. |
| `zenith_collection_tests` | Test executable | Collections, visibility rules and modes, collection-instance expansion, the version 3 document form and v2 migration, and undo patches of collection definitions. |
| `zenith_asset_reference_workflow_tests` | Test executable | Editor end-to-end GUID references: current-version scene saves, browser and external renames and moves followed by the watcher, the open scene following its file, automatic model reloads, and every checked-in scene loading through migration. |
| `zenith_asset_database_tests` | Test executable | Content hashing, asset GUIDs, `.meta` sidecars, and asset database scans, moves, external changes, and dependencies on real temporary directories. |
| `zenith_physics_tests` | Test executable | Rigid bodies, contacts, impulses, raycasts, mesh collision, and cleanup. |
| `zenith_physics_queries_tests` | Test executable | Filtered rays, shape casts, overlaps, and closest points; project collision layers; sensor trigger enter and exit order; the rigid body's layer field across schema versions. |
| `zenith_application_tests` | Test executable | Lifecycle ordering, deferred switching, failed-load cleanup, demo composition, controls, and reload behavior. |
| `zenith_project_runtime_tests` | Test executable | The game's command line, packaged-project discovery, gameplay selection and in-place module loading, `ProjectApplication` running `zenith_test_game_module`'s Play systems (and a `--scene` file outside the scene root), and verification of every project scene. |
| `zenith_package_asset_collisions` | CMake script test | `tests/packaging/PackageAssetCollisionTests.cmake`: a project asset at an engine runtime asset's path fails the configure unless overridden. |

Every executable, test, and shared library (the engine, SDL, and test modules)
is written to one runtime directory, `build/<config>/` with multi-config
generators and `build/` otherwise, so the loader finds the engine library
beside each executable. A target that calls a privately linked dependency
directly (volk, meshoptimizer) links its own copy of it.

The build publishes `src/` as the engine include root. Consumers include
headers as, for example, `engine/scene/Scene.hpp`. Runtime code uses the
executable-relative resource locator, which supports both multi-config build
trees and cleanly extracted release archives without embedding build paths.

Third-party libraries are pinned with CMake `FetchContent`:

| Dependency | Role |
| --- | --- |
| Vulkan Headers + Volk | Vulkan declarations and dynamic loader. |
| Vulkan Memory Allocator (VMA) | GPU buffer and image memory allocation. |
| SDL 3 (shared) | Window creation, Vulkan surface creation, events, keyboard, and mouse. |
| GLM | Vectors, matrices, quaternions, and transforms. |
| EnTT | Entity-component registry. |
| Jolt Physics | Collision detection and rigid-body simulation behind Zenith's physics API. |
| cgltf | `.gltf`/`.glb` parsing. |
| meshoptimizer | Import-time vertex/index ordering and LOD simplification. |
| Recast & Detour (recastnavigation) | Navigation mesh building and path queries behind `NavigationWorld`. |
| stb_image | Image decoding to RGBA pixels. |
| bc7enc_rdo | Offline BC7 (bc7enc) and BC5 (rgbcx) block compression for cooked textures. |
| glslang | Build-time GLSL-to-SPIR-V compiler. |
| shaderc + SPIR-V Tools | In-process shader compiler dependencies available to the engine target. |

## Subsystem docs

Each subsystem has its own short document. Feature details belong there; this
index changes only when a document is added, removed, or renamed.

Engine (`src/engine/`, `shaders/`):

- [Application and engine orchestration](engine/application.md): `src/main.cpp`, `Engine`, the `Logger`, and the `Application` lifecycle.
- [Window and input](engine/window-input.md): the SDL window, `InputManager`, and input action maps.
- [Runtime game UI](engine/ui.md): `GameUi`, the engine's game-facing UI API.
- [Audio](engine/audio.md): `AudioSystem` decoding, streaming, and mixing.
- [Scene](engine/scene.md): ECS scenes, reflection and component types, collections and visibility, prefabs, entity reference remapping, jobs, and additive scenes.
- [Game modules](engine/modules.md): the module ABI, loading and shadow copies, system scheduling, behaviours, and `Gameplay`.
- [Projects](engine/projects.md): the project manifest and the checked-in demo, showcase, and irradiance-room projects.
- [Assets](engine/assets.md): CPU asset types, import, streaming policy, texture cooking, GUIDs, and the asset database.
- [Platform services](engine/platform.md): writable locations, file reads, the file watcher, and child processes.
- [Physics](engine/physics.md): physics components, the character controller, `PhysicsWorld`, and the private Jolt runtime and debug draw.
- [Animation](engine/animation.md): clip playback, cross-fades, and the animator state machine.
- [Navigation](engine/navigation.md): navigation meshes from static colliders, paths, and agents.
- [Gameplay](engine/gameplay.md): reusable gameplay mechanisms such as the first-person player.
- [Graphics](engine/graphics.md): the renderer structure, Vulkan context, swapchain, frame resources, render graph, and pipelines.
- [Graphics scene passes](engine/graphics-passes.md): GPU-driven culling, AO, clustered and local-light shadows, fog, decals, particles, environment lighting, and `MeshPass`.
- [Graphics post-processing](engine/graphics-post.md): post-process, colour grading, SSR, bloom and exposure, motion blur, TAA, upscaling, and depth of field.
- [Shaders](engine/shaders.md): every GLSL file under `shaders/` and its stage.

Editor (`src/editor/`):

- [Editor overview](editor/overview.md): the composition root, `EditorApplication`, `EditorSession`, and the Info log.
- [Areas and workspaces](editor/areas-workspaces.md): area types, workspaces, dock layouts, and `workspaces.json`.
- [Viewports](editor/viewport.md): viewport panels and chrome, shading, scene gizmos, selection tools, snapping, the 3D cursor, and pick queries.
- [Scene editing, history, and live inspection](editor/scene-editing.md): `SceneEditing` diffs, undo history, and editing the running scene.
- [Build and IDE workflow](editor/build-ide.md): the Build panel, compiler output, IDE launching, and Build and Package.
- [Theme, preferences, and render settings](editor/theme-settings.md): Preferences, the Pixel Aero Night theme, and the render settings and Graphics panel.
- [Project Settings](editor/project-settings.md): the project's physics layers, input actions, and tags, and the inspector's choices from them.
- [Operators and keymaps](editor/operators.md): the operator registry and runner, keymaps, search, and the headless operator sets.
- [Menus](editor/menus.md): menus as data, popups, pie menus, and the default menus.
- [Game module hosting](editor/modules.md): loading a project's module, running its systems, hot reload, and the editor extension API.
- [Play, Simulate, and play targets](editor/play-mode.md): Simulate, time scale, the System Profiler, play targets, and the playback toolbar.
- [Inspector and Properties panels](editor/inspector.md): component sections, multi-entity editing, entity reference fields, and the Properties tabs.
- [Outliner](editor/outliner.md): the Hierarchy panel's display modes, filtering, and drag and drop.
- [Asset Browser and project assets](editor/asset-browser.md): the browser, `ProjectAssets`, the asset catalog, and file operations.
- [Editor smoke](editor/smoke.md): the `ZENITH_EDITOR_SMOKE` scenario, its sequential step runner, and how to add a step.

Game (`src/game/`):

- [Demo game](game/demo.md): the demo's gameplay registration, executable composition, and `ProjectApplication`.

## Runtime content

| Path | Purpose |
| --- | --- |
| `assets/sample.gltf` | Small importer and static-mesh-collider fixture. |
| `assets/basic_map.gltf` | Current room, materials, and prop geometry. |
| `assets/audio/*` | Demo interaction and ambient audio plus provenance notes. |
| `assets/textures/*.png` | Base-color, normal, and packed occlusion/roughness/metallic maps. |
| `assets/textures/SOURCE.md` | Texture provenance and channel/convention notes. |
| `assets/showcase/*` | Renderer showcase glTF, textures and HDR sky; provenance in `assets/showcase/SOURCE.md`. |
| `assets/irradiance_room/room.gltf` | Generated boxes of the irradiance-volume test interior (`tools/irradiance_room/`). |
| `assets/color_grading/warm_cinematic.cube` | Example 17^3 warm cinematic grade for `RenderSettings::colorLut` (`tools/color_grading/`). |
| `assets/baked/*` | Editor-baked derived lighting data, such as irradiance volumes (`.zvol`), grouped by scene. |

The source directory is copied as a whole to `build/assets/` for runtime use.

## Tests

| Path | Coverage |
| --- | --- |
| `tests/engine/` | Audio, asset importing, scene documents/hierarchy, and physics contracts. |
| `tests/editor/` | Settings, build output parsing, build runner and IDE launching, render-settings persistence and graphics-panel helpers, document state, command history, undoable scene editing and entity commands, operators and keymaps, prefab authoring and operators, areas and workspaces (registry, type switching, layout switching, per-area state, `workspaces.json` and its version, and legacy layout migration), the built-in area types (each drawn headless, multi-instance Asset Browser areas and primary promotion, and the default workspaces' layouts and round trip), asset browser, selection, camera behavior, and component editing helpers. |
| `tests/game/` | Application callback order, scene changes, cleanup, demo composition, controls, and reload behavior; the `zenith_game_demo` module loaded with `GameModule::load`, parity with the linked gameplay, authored character controllers playing in the game, the demo project playing through its module, and the game's project runtime (command line, packaged-project discovery, module loading, `ProjectApplication`, scene verification). |
| `tests/packaging/` | CMake script tests of packaging configure checks (`cmake -P`), such as project assets colliding with engine runtime assets. |
| `tests/capture/`, `tests/references/` | Reference capture driver, the showcase, irradiance-room and debug-draw capture manifests, and reference PNGs (`docs/RENDERER_VALIDATION.md`). |

The executables return a nonzero status on failed expectations so CTest can
report failures without another test framework. Test sources and registrations
are explicit in the `tests/*/CMakeLists.txt` files.
