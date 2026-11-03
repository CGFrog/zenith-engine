# Completed milestones

Part of the [repository guide](../REPOSITORY_GUIDE.md). Archived history: the
milestones of [Next steps](../NEXT_STEPS.md) that are complete, with their
items and exit criteria as written, plus the completed items of milestones
that still have open work. They are kept for reference and are not updated.

## Milestone 1: editor foundation

- [x] Add an `src/editor/` application target that depends on `zenith_engine`.
- [x] Keep editor panels, selection, projects, and commands out of
      `src/engine/` and `src/game/`.
- [x] Define `EditorSession` as the owner of editor-only state.
- [x] Add an ImGui docking shell with View, Edit, File, and Play menus.
- [x] Add persistent panel visibility and layout settings.
- [x] Add a status bar for diagnostics and the existing render statistics.
- [x] Document editor-to-engine dependency direction in `ARCHITECTURE.md`.

Exit criteria:

- The game and editor are separate executables using the same engine library.
- Closing or reopening panels does not affect the runtime scene.
- The existing game continues to build and run without editor dependencies.

## Milestone 2: scene document and persistence

- [x] Introduce stable scene entity IDs independent of EnTT entity values.
- [x] Define a versioned, human-readable scene document format.
- [x] Serialize names, hierarchy, local transforms, and component data.
- [x] Store asset references as project-relative paths or stable asset IDs,
      never transient runtime handles.
- [x] Validate duplicate IDs, invalid parents, hierarchy cycles, missing
      assets, and unsupported document versions.
- [x] Return descriptive load/save errors without partially replacing the
      active scene.
- [x] Implement New, Open, Save, Save As, and dirty-document tracking.
- [x] Use an atomic temporary-file-and-rename save path.
- [x] Add round-trip, malformed-input, missing-asset, and version tests.

Exit criteria:

- A scene can be saved, restarted, and loaded without observable changes.
- Failed loads preserve the currently open scene.
- Persistence tests are CPU-only and deterministic.

## Milestone 3: hierarchy and inspector

- [x] Add a hierarchy panel with selection, rename, create, duplicate, delete,
      and reparent operations.
- [x] Prevent cycles and preserve world transforms when reparenting by choice.
- [x] Add multi-selection without changing the core scene API unnecessarily.
- [x] Add an inspector panel for local transform and existing components.
- [x] Make component editors small, independently testable adapters.
- [x] Support adding and removing optional components.
- [x] Display invalid or missing asset references clearly.
- [x] Add tests for hierarchy mutation and component lifecycle behavior.

Exit criteria:

- Designers can construct and organize a scene without editing source files.
- All hierarchy operations preserve scene invariants.

## Milestone 4: command history

- [x] Define a narrow editor command contract with execute and undo behavior.
- [x] Route document mutations through the command history.
- [x] Implement undo/redo for entity creation, deletion, reparenting, renaming,
      component changes, and property edits.
- [x] Coalesce continuous transform or numeric-field drags into one command.
- [x] Define history behavior across Save, Open, New, and play-mode changes.
- [x] Add deterministic command and history-limit tests.

Exit criteria:

- Every destructive editor operation in milestones 2 and 3 is reversible.
- Undoing and redoing returns the serialized document to identical content.

## Milestone 5: editor viewport

- [x] Add a renderer-owned offscreen viewport target with explicit resize and
      lifetime handling.
- [x] Present the viewport texture in an ImGui panel without exposing Vulkan
      details to editor code.
- [x] Add an editor camera with orbit, pan, fly, focus, and configurable speed.
- [x] Route viewport input through the existing capture model.
- [x] Add entity picking using a deterministic ID attachment or bounded CPU
      ray test; choose after measuring both approaches.
- [x] Add selection outline or another low-cost selection indicator.
- [x] Integrate translate, rotate, and scale gizmos.
- [x] Keep gizmo operations in local/world modes with snapping controls.
- [x] Treat one gizmo drag as one undoable command.

Exit criteria:

- Clicking visible geometry selects the correct entity.
- Viewport resizing and docking do not leak or recreate unrelated resources.
- Transform editing works from both the inspector and gizmos.

## Milestone 6: project and asset browser

- [x] Define a small project configuration containing asset and scene roots.
- [x] Add an asset browser for models, textures, audio, and scene files.
- [x] Move directory scanning and metadata extraction outside rendering code.
- [x] Add file-change detection with explicit, recoverable reload behavior.
- [x] Support dragging models into the viewport and assets onto compatible
      inspector fields.
- [x] Cache thumbnails asynchronously without blocking frame rendering.
- [x] Report import and reload errors in an editor diagnostics panel.

Exit criteria:

- A model can be found, placed, saved, and restored entirely through the UI.
- Asset refresh work cannot invalidate resources still used by an in-flight
  frame.

## Milestone 7: edit and play modes

- [x] Define explicit Edit, Play, and Paused states in the editor layer.
- [x] Instantiate a separate runtime scene when entering Play.
- [x] Keep physics and gameplay mutations out of the editable document.
- [x] Restore the editor scene and selection when leaving Play.
- [x] Add play, pause, single-step, and stop controls.
- [x] Define which runtime changes, if any, may be applied back deliberately.
- [x] Test repeated mode transitions and restoration after entity deletion.

Exit criteria:

- Stopping play always restores the exact pre-play serialized scene.
- Repeated mode transitions do not leak physics, audio, or GPU resources.

## Milestone 8: renderer follow-up

- [x] Cache render packets and update them only when topology, material, or
      transforms change; benchmark before and after.
- [x] Add dirty transform propagation to avoid full hierarchy recomputation.
- [x] Replace synchronous upload command pools and queue-idle waits with a
      reusable staging/upload context.
- [x] Implement true downsampled bloom before exposing higher bloom presets.
- [x] Keep temporal antialiasing unavailable until motion vectors and automated
      ghosting and stability captures exist; expose only Off and true FXAA.
- [x] Add cascade visualization and tune split blending and bias controls.
- [x] Add texture mip generation and choose anisotropy from device limits.
- [x] Prototype indirect/bindless submission only behind capability checks;
      retain the compatible instanced path and adopt the prototype only if the
      2,000-object benchmark improves materially.
- [x] Add deterministic screenshot comparisons for color-space, shadow, and
      anti-aliasing regressions.

Exit criteria:

- No default-quality change regresses median or p95 CPU/GPU frame time beyond
  the agreed measurement tolerance on the same machine and driver.
- Every quality option names only behavior that is actually implemented.

## Milestone 9: editor hardening

- [x] Add unsaved-change prompts for close, open, and new operations.
- [x] Add autosave/recovery without overwriting the primary document.
- [x] Add copy/paste and duplicate support based on serialized fragments.
- [x] Add keyboard shortcuts with conflict detection and discoverable labels.
- [x] Add structured editor diagnostics and actionable error messages.
- [x] Add a headless editor-document test suite to CI.
- [x] Add bounded Vulkan smoke tests for viewport resize and play transitions.
- [x] Document a minimal level-authoring tutorial.

Exit criteria:

- Common failure paths preserve user work.
- A new contributor can build the editor and author a simple level from the
  repository documentation.

## Milestone 10: runtime projects and authored scene flow

- [x] Move the reusable project manifest and parser from the editor into an
      engine project subsystem while keeping editor workflow policy in
      `src/editor/`.
- [x] Introduce project manifest version 2 with a display name, asset root,
      scene root, and project-relative startup scene.
- [x] Continue reading version 1 projects and upgrade them when saved. Report a
      descriptive runtime error when a project has no startup scene.
- [x] Allow game executables to open a project with `--project`, load its
      startup scene through `SceneDocument`, and transition between authored
      scenes without hard-coded asset paths.
- [x] Keep gameplay component creation and scene-selection policy in the game
      layer.
- [x] Test path containment, missing startup scenes, failed transitions,
      version compatibility, and preservation of the active scene after a
      failed load.

Exit criteria:

- An editor-authored project can be launched by a game and play its configured
  startup scene without recompilation.
- Scene transitions use project-relative references, and a failed transition
  leaves the current scene running.

## Milestone 11: action input and gamepad support

- [x] Preserve physical device state in `InputManager` and add reusable
      `InputActionMap`, `InputBinding`, and `InputActionState` APIs above it.
- [x] Support keyboard keys, mouse buttons and axes, SDL gamepad buttons,
      sticks, and triggers.
- [x] Expose pressed, released, held, one-dimensional, and two-dimensional
      action values.
- [x] Handle controller connection changes, dead zones, input capture, fixed
      update edge buffering, and active-device changes deterministically.
- [x] Store mappings in game or project configuration rather than assigning
      gameplay meaning inside the engine.
- [x] Convert the demo to Move, Look, Jump, Sprint, Interact, Pause, and Reload
      actions.
- [x] Add CPU tests for chords, dead zones, rebinding, hot-plugging, capture,
      and fixed-step press retention.

Exit criteria:

- The demo is fully playable with keyboard and mouse or a standard gamepad.
- Gameplay code consumes actions rather than physical keys and buttons.

## Milestone 12: runtime game UI

- [x] Add an engine-owned, resolution-independent game UI layer that is
      separate from editor ImGui.
- [x] Expose a `GameUiContext` through the application context with text,
      image, panel, button, focus, anchoring, scaling, clipping, and draw-order
      operations.
- [x] Add font assets and use existing texture handles without exposing ImGui
      or Vulkan types to game code.
- [x] Render game UI after tone mapping and before presentation.
- [x] Integrate pointer interaction and action-based keyboard and gamepad
      navigation.
- [x] Replace the demo's direct ImGui UI with a title screen, HUD, pause menu,
      and audio, rendering, and input settings screens.
- [x] Test layout, focus traversal, hit testing, scaling, action handling, and
      settings persistence without a GPU. Add screenshot coverage for final
      composition and color space.

Exit criteria:

- Game code can implement menus and HUDs without including ImGui or Vulkan
  headers.
- UI remains usable at 16:9, ultrawide, and high-DPI viewport sizes.

## Milestone 13: animated and alpha glTF content

- [x] Extend CPU assets with joints, weights, skins, inverse-bind matrices,
      animation clips, channels, and opaque, masked, and blended material
      modes.
- [x] Support glTF step, linear, and cubic-spline animation sampling, looping,
      playback speed, clip selection, and two-clip cross-fades.
- [x] Add serializable animation and skin scene components plus editor preview
      controls.
- [x] Upload per-instance skin palettes without duplicating immutable mesh
      assets.
- [x] Keep opaque and masked primitives on the optimized render path,
      alpha-test masked shadows, and sort blended primitives back-to-front.
- [x] Respect glTF alpha cutoff and double-sided settings. Blended materials do
      not cast shadows in this milestone.
- [x] Add importer and animation-evaluation tests plus bounded Vulkan captures
      for skinning, masked edges, blending order, and editor preview.

Exit criteria:

- A standard animated glTF character can be imported, previewed, saved,
  reloaded, and rendered correctly.
- Common cutout and blended environment assets render with the expected order,
  culling, and shadow behavior.

## Milestone 14: Windows and Linux release packaging

- [x] Resolve shaders, project data, and assets through a relocatable,
      executable-relative resource locator instead of build-directory paths.
- [x] Add CMake install rules and package targets that produce a Windows ZIP
      and Linux `.tar.gz` archive.
- [x] Package the game executable, compiled shaders, selected project content,
      required runtime files, licenses, and a versioned content manifest.
- [x] Reject missing, duplicate, escaping, and case-conflicting asset paths
      before producing an artifact.
- [x] Add Windows and Linux CI jobs for configure, build, CPU tests, and Release
      packaging. Run bounded Vulkan smoke tests where a suitable runner exists.
- [x] Verify each artifact from a clean extraction directory, including a path
      containing spaces, without access to the source or build tree.
- [x] Document release creation, supported runtime dependencies, and artifact
      contents.

Exit criteria:

- CI produces self-contained Windows and Linux archives that launch the
  configured project after clean extraction.
- Each archive passes the automated CPU suite and a bounded Vulkan smoke run on
  supported release hardware.

## Milestone 15: asset lifetime and content integrity

- [x] Replace append-only asset storage with explicit strong references or
      leases at scene, editor-preview, audio-voice, and renderer boundaries.
- [x] Recycle asset slots with generation increments so stale handles always
      fail resolution instead of aliasing newly loaded content.
- [x] Add an eviction policy with configurable memory budgets and deterministic
      manual collection for tests and tools.
- [x] Defer GPU and audio-resource destruction until every in-flight consumer
      has finished, without device-wide or queue-wide idle waits.
- [x] Preserve the current recoverable reload behavior while separating the
      old generation from future lookups.
- [x] Verify packaged content against `content-manifest.json` in a CPU-only
      validation mode before initializing window, audio, or Vulkan services.
- [x] Report asset ownership, generation, CPU/GPU byte estimates, and eviction
      failures in editor diagnostics.
- [x] Add deterministic tests for stale handles, shared ownership, reload,
      eviction, budget pressure, and deferred destruction.

Exit criteria:

- Repeated scene and project transitions do not grow retained asset memory once
  collection has completed.
- Releasing or reloading an asset cannot invalidate a live scene or an
  in-flight frame, and corrupted packaged content fails before engine startup.

## Milestone 16: asynchronous loading and additive scenes

- [x] Add a bounded engine job system with cancellation, completion queues,
      explicit shutdown, and deterministic single-threaded test execution.
- [x] Split scene loading into background-safe parsing and a main-thread commit
      phase for ECS, physics, audio, and GPU-facing state.
- [x] Expose progress, cancellation, and descriptive failures without allowing
      a partial load to replace or mutate the active scene.
- [x] Add additive scene instances with stable instance IDs, explicit ownership,
      deterministic unload order, and cross-scene reference validation.
- [x] Keep gameplay transition and loading-screen policy in the application
      layer; engine APIs provide only loading and scene-instance mechanisms.
- [x] Prioritize and cancel asset requests as additive scenes enter and leave
      the streaming radius.
- [x] Add CPU stress tests for cancellation races, shutdown during loading,
      failed commits, repeated additive load/unload, and asset collection.
- [x] Add a bounded Vulkan smoke scenario that streams content while frames are
      in flight and confirms that presentation remains responsive.

Exit criteria:

- A large authored scene can load without blocking frame presentation for its
  full parse duration.
- Failed or cancelled loads leave all committed scene instances and their asset
  ownership unchanged.

## Milestone 17: prefabs and reusable scene composition

- [x] Define a versioned prefab document using stable entity IDs and
      project-relative asset or nested-prefab references.
- [x] Support nested prefab instances with cycle detection, deterministic ID
      remapping, and clearly defined local versus inherited hierarchy.
- [x] Store property and component overrides separately from the immutable
      prefab source rather than copying an opaque serialized scene fragment.
- [x] Add editor operations to create, instantiate, unpack, apply, revert, and
      inspect prefab overrides through undoable commands.
- [x] Propagate source changes while preserving valid instance overrides and
      report orphaned overrides as actionable diagnostics.
- [x] Allow scenes and additive scene layers to share prefab definitions without
      coupling runtime instantiation to editor state.
- [x] Add round-trip, nesting, override, source-update, missing-source, cycle,
      duplicate-ID, and undo/redo tests.

Exit criteria:

- A reusable authored object can be placed in several scenes, updated once,
  and refreshed without losing intentional per-instance changes.
- Prefab failures preserve both the open document and previously valid runtime
  instances.

## Milestone 18: scalable lighting and environment rendering

- [x] Move user-facing renderer and post-process choices into serializable
      project or scene configuration with capability validation and safe
      fallbacks.
- [x] Support multiple local point and spot lights through a measured clustered
      or tiled light-selection path rather than per-object unbounded loops.
- [x] Add image-based environment lighting with prefiltered specular data and a
      reusable offline or cached preprocessing step.
- [x] Add reflection probes with explicit influence volumes and deterministic
      blending; keep probe baking policy in editor tooling.
- [x] Introduce GPU timestamp scopes and per-pass CPU/GPU budgets in benchmark
      output for shadow, opaque, blended, bloom, UI, and upload work.
- [x] Keep the compatible instanced submission path and gate more advanced GPU
      submission behind capability checks and measured benefit.
- [x] Add deterministic reference captures for local lights, probes, HDR
      environment color, transparency, and every exposed quality fallback.

Exit criteria:

- A representative indoor/outdoor scene can use many local lights and
  environment reflections within documented frame and memory budgets.
- Every saved quality option either produces its named behavior or reports and
  selects an explicit supported fallback.

## Milestone 19: production glTF and texture pipeline

- [x] Import and evaluate glTF morph targets, including animation weight
      channels, bounded active-target selection, and editor preview controls.
- [x] Support Meshopt- and Draco-compressed mesh input through optional,
      isolated decoder dependencies with clear missing-decoder diagnostics.
- [x] Add KTX2/Basis Universal texture import and choose supported GPU formats
      through renderer capabilities while retaining an uncompressed fallback.
- [x] Add an offline content build step that produces deterministic derived
      assets keyed by source content, import settings, tool version, and target
      platform.
- [x] Store derived data outside source asset folders and make invalidation,
      inspection, and rebuilding available to the editor and package pipeline.
- [x] Validate vertex, index, morph, texture, and decoded-size limits before
      allocating memory for untrusted content.
- [x] Add conformance fixtures, malformed-input tests, cache reproducibility
      checks, and bounded Vulkan captures for every supported fallback path.

Exit criteria:

- Common production glTF characters and environments import without manual
  decompression or texture conversion.
- Identical source content and settings produce byte-identical derived assets,
  and corrupted inputs fail within documented resource limits.

## Milestone 20: gameplay scripting and extension boundary

Decided 2026-10-04 (see the [gameplay roadmap](../GAMEPLAY_ROADMAP.md#decisions)):
gameplay is C++ in hot-reloaded game modules. Per-entity logic is a
`Behaviour`; bulk logic stays in systems. No embedded scripting runtime is
planned.

Exit criterion, met by the FPS template ([gameplay roadmap item
C6](gameplay-roadmap-2026.md#c6-fps-template-and-tutorial),
`zenith_fps_template_module_build`): a packaged project implements a small
gameplay loop in its game module without recompiling the engine or accessing
subsystem internals.

- [x] Keep gameplay modules project-owned and make engine registration
      explicit (`ModuleRegistrar`); modules reach services only through the
      contexts they are handed.
- [x] Serialize behaviours as typed component properties with schema versions,
      migrations, and descriptive validation; a module that fails to load keeps
      its components as preserved JSON.
- [x] Define deterministic create, enable, start, update, fixed-update,
      late-update, disable, and destroy ordering (`BehaviourScheduler`).
- [x] Add editor property inspection and state-preserving hot reload
      (`GameModuleHost`).
- [x] Add lifecycle, hot-reload, and serialization tests
      (`zenith_behaviours`, `zenith_game_module_host`).
- [x] Isolate a behaviour that throws, so it is disabled with a diagnostic
      while the rest of the game keeps running (`BehaviourScheduler::callHook`,
      gameplay roadmap E7). Systems, timers, and command-buffer callbacks were
      still unguarded at the time; see the [roguelike readiness
      review](../ROGUELIKE_READINESS_REVIEW.md), Critical item 3. They are guarded
      now ([Game modules](../engine/modules.md#fault-isolation)).
- [x] Define the gameplay-facing API boundary
      ([Gameplay API](../engine/gameplay-api.md), 2026-11-02): the supported
      headers, checked at build time to stay free of SDL, Vulkan, Jolt, ImGui
      and editor types. EnTT stays public by decision, so the original wording
      ("without exposing EnTT") was dropped. `InputState.hpp` was split from
      `InputManager.hpp` to keep SDL out, and `SystemContext::physics` and
      `ContentServices::assets()` replaced modules' use of
      `SystemContext::application` (module API version 15).

## Milestone 21: platform, accessibility, and release operations

- [x] Add a localization catalog with stable message IDs, UTF-8 text shaping,
      font fallback, plural rules, and runtime locale selection.
- [x] Add remappable keyboard, mouse, and gamepad controls with multiple saved
      profiles, conflict resolution, and accessible hold/toggle alternatives.
- [x] Add subtitle/caption timing, scalable UI and text, high-contrast themes,
      reduced-motion options, and color-independent gameplay indicators.
- [x] Persist versioned user settings and save data through platform-appropriate
      writable locations with atomic migration and recovery behavior.
- [x] Produce signed installer-ready Windows artifacts, Linux desktop metadata,
      and a macOS application bundle after the corresponding CI runner and
      Vulkan portability path pass the same package verification contract.
- [x] Add structured crash reports and privacy-preserving diagnostic bundles
      that never upload or transmit data without explicit application policy.
- [x] Generate release notes and provenance metadata from the exact source,
      dependency, toolchain, and content-manifest revisions used by CI.
- [x] Test locale switching, input-only navigation, settings migration, clean
      user profiles, offline startup, and upgrade/rollback artifact behavior.

Exit criteria:

- A keyboard-only or gamepad-only user can install, configure, understand, and
  play the packaged demo without editing files or relying on color alone.
- A release can be reproduced and diagnosed from its archived provenance while
  preserving user data across supported upgrades and rollbacks.

## Milestone 22: renderer validation and hardening (completed items)

The open items and the exit criteria stay in [Next steps](../NEXT_STEPS.md#milestone-22-renderer-validation-and-hardening).

The renderer modernization (bindless materials, GPU-driven culling, BC
textures, packed vertex streams, LODs, IBL, clustered lighting, depth prepass,
GTAO, Hi-Z SSR, TAAU, bloom, AgX, and auto exposure) has run on one GPU, without
Vulkan validation, on two small scenes. Harden it before adding features.

- [x] Install the Vulkan SDK and run the demo and
      `projects/showcase.zenith-project` with `VK_LAYER_KHRONOS_validation` and
      synchronization validation on both the CPU and GPU-driven paths, with and
      without high quality and dynamic resolution. Fix every message.
- [x] Add a `vulkan;validation` CTest smoke label that fails on any validation
      message when the layer is present and is skipped otherwise.
- [x] Add built-in frame capture (an environment variable that writes the
      backbuffer of a chosen frame to PNG) plus a fixed showcase camera, so
      visual checks no longer depend on RenderDoc and window focus.
- [x] Add deterministic showcase reference captures for TAAU ghosting, masked
      prepass edges, GTAO, SSR, bloom, and exposure, and compare them in CI where
      a GPU runner exists.
- [x] Extend the showcase with an animated skinned glTF and a mirror-smooth
      metal surface to cover skinned motion vectors, packed skin streams, and
      SSR on metals.
- [x] Audit silent fallbacks like the upload-backpressure bug fixed in
      `5820789`: count deferred mesh and texture uploads in `RenderStats` and log
      once when an asset stays deferred for more than a bounded number of frames.

## Milestone 23: lighting fidelity

Global illumination is the largest remaining fidelity gap against mainstream
engines; local-light shadows and volumetrics follow.

- [x] Add baked irradiance probe volumes (a grid with editor baking and
      visibility-aware interpolation) feeding indirect diffuse in `mesh.frag`.
      Evaluate screen-space GI or ray-traced probe updates later as optional
      tiers.
- [x] Implement a local-light shadow atlas (`allocateLocalShadowAtlas` exists
      but is unused): spot lights first, then point lights, with cached static
      casters and a per-frame update budget.
- [x] Cull shadow cascades on the GPU by reusing `GpuDrivenPass`.
- [x] Blend reflection probes per pixel through the cluster grid, with box
      projection, instead of once at the camera position.
- [x] Add froxel volumetric fog that reuses the cluster grid and shadow maps.
- [x] Store specular colour in the prepass normal target so SSR tints metal
      reflections correctly.

Exit criteria:

- A representative interior shows plausible bounce lighting without manual
  fill lights, and every new technique has an Off fallback and its own GPU scope
  and budget.

## Milestone 24: post-processing, materials, and effects

- [x] Add depth of field, motion blur (from the existing motion vectors), colour
      grading LUTs, vignette, and film grain as individually selectable passes.
- [x] Offer FSR or DLSS behind capability checks alongside TAAU.
- [x] Support `KHR_materials_clearcoat`, `transmission`, `sheen`, and `specular`.
- [x] Add clustered decals and a GPU particle system with sorting and lighting.

Exit criteria:

- Each pass and material feature has a reference capture, a GPU scope, and a
  documented cost on the reference hardware.

## Milestone 25: renderer performance follow-ups

Tracked against `PERFORMANCE_ROADMAP.md`, whose GPU-driven visibility and
image-quality steps are now largely implemented.

- [x] Replace the per-mip depth-pyramid dispatches with a single-pass
      downsampler, and build the pyramid once per frame on the GPU-driven path.
- [x] Reduce GTAO cost (0.6 ms at full resolution on the reference GPU): default
      to half resolution or adopt a cheaper sampling pattern.
- [x] Decide whether GPU-driven submission becomes the default using the
      showcase and a large draw-call-heavy scene; it is currently slower on both
      small test scenes.
- [x] Stream texture mip ranges through the derived-asset cache and evaluate
      asynchronous compute for clustering, pyramid, and bloom.
- [x] Clean up: deduplicate the mip-chain blit code in `EnvironmentPass` and
      `MeshPass`, remove the unused post-process descriptor bindings 3–6, stop
      `Renderer.cpp` from mirroring `MeshPass` decisions for the render-graph
      model, and make FXAA read the composited image.

Exit criteria:

- No change regresses median or p95 GPU frame time on the demo, the showcase,
  or the 4,000-object benchmark beyond measured noise.

## Test suite follow-ups (completed)

- [x] Record editor edits without serializing the whole scene twice per
      command (`SceneEditing::perform`); large scenes pay for it on every edit,
      and it bounds the size of `zenith_scene_editing`'s large-scene test.
      Done by gameplay roadmap D4 (`SceneEditing::captureEntities`).
- [x] Additive-scene tests (2026-11-02, `tests/engine/AdditiveSceneTests.cpp`):
      broken and dangling references, failed loads that publish nothing,
      unload ownership with shared assets, duplicate loads, unload order,
      cancelling a load, and `clear`.
- [x] `JobSystem` tests (2026-11-02, `tests/engine/JobSystemTests.cpp`):
      ordering, bounded submission, exceptions, cancellation, and shutdown or
      destruction while busy. They found that `reprioritize` let a job jump
      ahead of older jobs of the same priority; the queue now breaks ties by
      submission order.
- [x] `ThumbnailCache` and outliner tests (2026-11-02,
      `tests/editor/ThumbnailCacheTests.cpp`, `tests/editor/OutlinerTests.cpp`):
      downsampling, request/find/invalidate, worker shutdown, filter prefixes,
      and reparent and collection drops. They found that some thumbnails came
      out 95 texels on the long side instead of 96; sizes are now integer
      arithmetic.
- [x] Cook optimized meshes and LOD chains into the derived-asset cache
      (2026-11-02, `src/engine/assets/CookedMesh.*`, `zenith_cooked_meshes`):
      a cache hit skips meshoptimizer (about 4.5x faster model loads in a
      Debug build). See [Assets](../engine/assets.md).
