# Editor workspace guide

The editor is a client of `zenith_engine`. Engine and game code must never
depend on this subtree.

## Directory map

| Path | Responsibility |
| --- | --- |
| `main.cpp` | Editor executable composition root. |
| `application/` | Editor application lifecycle and the UI/session coordinator, including Play mode and its targets (`PlayTargets`: game views, input routing, mouse capture, the standalone game process; wired in `EditorPlayback.cpp`). |
| `areas/` | Blender-style areas and workspaces: the area type registry, per-workspace areas and their ImGui dock layouts, the default workspaces, legacy layout migration, and `workspaces.json`. |
| `document/` | Scene document state, persistence workflow, undo/redo history, prefab authoring (`PrefabAuthoring`), per-scene editor data such as the 3D cursor (`SceneEditorData`, a sidecar beside the scene file), and the runtime-to-edit diff behind Apply to Edit Scene (`RuntimeApply`). |
| `modules/` | Game module hosting: loading the project's module, running its systems, hot reload, and the header-only editor extension API modules implement. |
| `menus/` | Menus as data: the `MenuRegistry` of named menus, entries, and dynamic sections (owner tokens for module cleanup), the searchable popup and menu-item drawing, pie menus (`PieMenu`, `wm.call_pie`), `wm.call_menu`, and the default Add, Add Component, and viewport pie menus. |
| `interaction/` | Editor camera, picking, selection, gizmos, component editing widgets (including entity reference fields), and the entity eyedropper. |
| `operators/` | Operator registry and runner (poll, execute, undo, Adjust Last Operation; operators run on the runtime scene without undo in Play and Simulate), headless object, component, collection, visibility (hide), outliner, and prefab operators, the headless play operators (time scale, Simulate systems, profiler reset, Apply to Edit Scene), the preferences and Build and Package operators, the per-region keymap (key and viewport mouse chords, Press/DoubleTap/Hold/Release/Click/Drag triggers) with `keymap.json` overrides and its input dispatch (`KeymapDispatch`), F3 search, and the keymap editor. |
| `panels/` | One class per dockable window, drawn from an `EditorContext`, plus the floating Preferences window; `panels/outliner/` holds the outliner's display-mode tree model, orphan data, and row widgets. |
| `project/` | Project tooling: the shared asset service (`ProjectAssets`: catalog, watchers, change dispatch, favourites), asset catalog, thumbnails, bakers, the build runner, Build and Package (`PackageRunner`: the planned release commands and their runner), compiler-output parsing, IDE launching, the Project Hub (`ProjectHub`), the recent projects list, and the project template generator (`ProjectTemplate`, from `templates/`). |
| `settings/` | Persistent editor preferences (`settings.ini`: tools, theme tuning, UI scale, pie menus, autosave), including the component inspector's section order and collapse state (legacy panel flags are only read, for migration) and the systems chosen for Simulate. |
| `theme/` | The "Pixel Aero Night" look: palette, its tuning (`ThemeParameters`, passed explicitly; one look only), art-pixel metrics, pixel fonts, the procedural wallpaper and frosted glass, window chrome, themed widget wrappers, and pixel-art icons. |
| `viewport/` | 3D viewport tools: scene overlays (`SceneGizmoDraw`: light, camera, collider, volume, and selection gizmos and name labels recorded into a view's debug list, free of ImGui so they also run in engine-hosted tests) and selection (GPU-first picking with CPU fallback through `ViewportPicker`/`ViewportPickProvider`, the modal box and lasso tools, view framing, and the select and local view operators), snap target queries (`SnapQueries`), and the 3D cursor (`Cursor3D`: placement, drawing, and its operators). |

## Local rules

- Reusable runtime behavior belongs in `src/engine/`; authoring policy and ImGui
  workflows stay here.
- Keep document operations usable without a window or GPU so they remain easy
  to test.
- Add editor tests under `tests/editor/` and update the explicit target source
  lists in the root `CMakeLists.txt`.
- `EditorSession` is integration code. Move reusable document or interaction
  behavior into its focused folder rather than growing the coordinator.
- Component inspectors come from the engine's component registry. Prefer
  better field presentation hints over a custom editor; register a custom
  editor in `interaction/EntityPropertiesView` (shared by the Inspector and
  Properties panels) only when reflected fields cannot express the UI; a game
  module's inspector override (`ComponentEditorOverrides`) takes precedence. The
  view edits several selected entities at once: a custom editor edits only the
  component it is given, and the view copies the document keys it changed to
  the others.
- Every user action is a registered `Operator` with a stable id
  (`object.duplicate`, `file.save`). Menus, buttons, and keys invoke it by id
  (`EditorContext::invoke`); add default keys in `operators/DefaultKeymap.cpp`.
  Operators that only edit the document belong in `operators/` and must run
  headless; ones needing the window are registered by `EditorSession`. An
  `Undo` operator must not commit history itself; the runner records it.
  An operator that creates objects takes a `location` property filled from
  the 3D cursor by `Operator::invokeDefaults`, so new objects spawn at the
  cursor and Adjust Last Operation can still change where.
- A menu is registered data in the session's `MenuRegistry`
  (`menus/DefaultMenus.cpp` for the editor's own), never hard-coded ImGui
  items: entries name operators, and content that changes (component types)
  comes from a dynamic section filled when the menu is shown. Open a popup
  menu with `wm.call_menu {"menu": id}`. Anything that registers menu content
  from module code uses its own `MenuOwner` and removes it before the module
  unloads.
- A new window is an `EditorPanel` in `panels/`. Panels receive everything
  through `EditorContext`, record scene changes through `SceneEditing` so they
  are undoable, and never reach into other panels.
- In Play and Simulate, `SceneEditing` is the runtime scene's
  (`SceneEditing::runtime()`): the same capture/commit calls edit the running
  scene live and record nothing, so never write to `CommandHistory` directly
  and never gate scene edits on the mode in a panel. An operator that only
  makes sense for the document (files, prefabs, collections, editor-only
  state) polls `editMode()`; the runner then explains the refusal. Only
  `SceneEditing::applyToEditScene` (`play.apply_to_edit`) carries runtime
  state back into the edit history.
- A panel becomes visible to users by adding an area type to
  `builtInAreaTypes` (`application/EditorAreaTypes.cpp`), which
  `EditorSession::registerAreaTypes` registers: a panel with per-window state
  uses a factory and `AreaInstancing::Multiple`, taking shared services
  through `EditorAreaServices`; a panel the session must own (because other
  workflows use it) is a session member shown with
  `AreaType::shared("id", panel)`. The type id is persisted in
  `workspaces.json` and window names, so never rename it; add the type to a
  default workspace in `areas/DefaultWorkspaces.cpp` if it should be open for
  new users, and bump `currentWorkspaceFileVersion` (`areas/WorkspaceFile.hpp`)
  whenever the defaults change so saved workspaces pick them up. A per-area
  panel can keep view state with its area by overriding
  `EditorPanel::areaState`/`restoreAreaState`. Areas, workspaces, and layouts
  are user data, never scene data.
- Colours come from `theme/EditorPalette` and sizes from the art-pixel grid
  (`editorArtPixel`); never write colour literals or rounded corners in
  panels. Use `widgets::Button`, `widgets::SectionHeader`, and the other
  `theme/EditorWidgets` wrappers for themed controls; they keep ImGui's ids,
  so tests can address them by label. Area windows get their glass frame from
  `ThemeChrome` in `drawAreaWindow`; panels draw only their contents.
- The viewport hide (`VisibilityComponent::hidden`, collection `visible`) is
  editor-only: anything that picks or selects in the viewport uses
  `SceneVisibility` in `Editor` mode, and Play mode renders in `Runtime` mode.
- Automated editor runs must set `ZENITH_EDITOR_SETTINGS_DIR` so they never
  read or replace the user's settings directory. They also set
  `ZENITH_EDITOR_SMOKE`, which keeps the Project Hub away.
- State that must survive a project switch (the ImGui theme and fonts, the
  recent projects) belongs to `EditorApplication`; everything per project
  belongs to `EditorSession`, which is destroyed when the user switches.
- Viewports are multiple-instance areas registered with the session's
  `ViewportRegistry`. Code acting on "the viewport" (view3d operators, modal
  transforms, the status bar) uses `ViewportRegistry::active()` and must
  handle it being null; never reach a viewport through a session member.
  Viewport shading goes to the viewport's render view through
  `setViewSettings`, never into the project's render settings.
- A viewport playing the game (`ViewportPanel::gameView`) belongs to the
  game: it shows the game camera and takes no editor input, so new viewport
  tools must skip it. Player input reaches module systems only through
  `EditorSession::gameInput`, and game views hold the mouse only through the
  session's `mouseCapturePolicy`.
- A click that picks an entity in a viewport or the outliner must first
  check `EditorContext::eyedropperArmed()` and hand the entity to
  `pickEyedropperEntity` instead of changing the selection.
- Never cache a game module's `ComponentType` pointers or registries across
  frames: read them from `EditorDocument::options()`. Before a module unloads,
  `GameModuleHost` releases its registries, systems, operators, panels,
  sidebar tabs, inspector overrides, and menus and replaces (never clears) the
  scene registry; a new holder of module code must be released there too.
- A new editor extension point for modules goes in the header-only
  `modules/EditorModuleApi.hpp` (bump `ZENITH_EDITOR_MODULE_API_VERSION`),
  is registered and released by `GameModuleHost`, is used by
  `tests/editor/modules/EditorTestModule.cpp`, and is documented in
  `docs/EDITOR_EXTENSIONS.md`.
