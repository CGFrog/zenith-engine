# Editor workspace guide

The editor is a client of `zenith_engine`. Engine and game code must never
depend on this subtree.

## Directory map

| Path | Responsibility |
| --- | --- |
| `main.cpp` | Editor executable composition root. [Overview](../../docs/editor/overview.md) |
| `application/` | Editor lifecycle, appearance, and the session coordinator, including Play mode. [Overview](../../docs/editor/overview.md), [Play](../../docs/editor/play-mode.md) |
| `areas/` | Blender-style areas, workspaces, dock layouts, and `workspaces.json`. [Areas](../../docs/editor/areas-workspaces.md) |
| `document/` | Scene document state, persistence, undo history, and prefab authoring. [Scene editing](../../docs/editor/scene-editing.md) |
| `modules/` | Game module hosting, hot reload, and the editor extension API. [Modules](../../docs/editor/modules.md) |
| `menus/` | Menus as data: the menu registry, popups, pie menus, and default menus. [Menus](../../docs/editor/menus.md) |
| `interaction/` | Editor camera, picking, selection, gizmos, and component editing widgets. [Inspector](../../docs/editor/inspector.md), [Viewports](../../docs/editor/viewport.md) |
| `operators/` | Operator registry and runner, headless operator sets, keymaps, and F3 search. [Operators](../../docs/editor/operators.md) |
| `panels/` | One `EditorPanel` per dockable window, plus the Preferences window. [Areas](../../docs/editor/areas-workspaces.md) |
| `project/` | Project tooling: asset service and catalog, build and package runners, Project Hub. [Asset Browser](../../docs/editor/asset-browser.md), [Build](../../docs/editor/build-ide.md) |
| `settings/` | Persistent editor preferences and per-user editor state files. [Settings](../../docs/editor/theme-settings.md) |
| `theme/` | The "8-bit Blender" look: palette, metrics, pixel font, window chrome, and themed widgets. [Theme](../../docs/editor/theme-settings.md) |
| `smoke/` | The editor smoke: sequential steps, ordered in `EditorSmokeScenario.cpp`. [Smoke](../../docs/editor/smoke.md) |
| `viewport/` | 3D viewport tools: scene overlays, selection tools, snapping, and the 3D cursor. [Viewports](../../docs/editor/viewport.md) |

## Local rules

- Reusable runtime behavior belongs in `src/engine/`; authoring policy and ImGui
  workflows stay here.
- Keep document operations usable without a window or GPU so they remain easy
  to test.
- Add editor tests under `tests/editor/` and register them in
  `tests/editor/CMakeLists.txt`; editor sources are listed in
  `src/editor/CMakeLists.txt`.
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
  headless; ones needing the session reach it through the narrow
  `application/EditorSessionCommands` interfaces. Every family is one line in
  `operators/EditorOperatorFamilies.cpp`, never a call in `EditorSession`. An
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
- A panel becomes visible as an area type in `builtInAreaTypes`; never rename
  a type id. See [Making a panel visible as an area type](../../docs/editor/areas-workspaces.md#making-a-panel-visible-as-an-area-type).
- Colours come from `theme/EditorPalette` and sizes from the art-pixel grid
  (`editorArtPixel`); never write colour literals or rounded corners in
  panels. Use `widgets::Button`, `widgets::SectionHeader`, and the other
  `theme/EditorWidgets` wrappers for themed controls; they keep ImGui's ids,
  so tests can address them by label. Area windows get their header strip and
  pane from `ThemeChrome` in `drawAreaWindow`; panels draw only their contents.
- The viewport hide (`VisibilityComponent::hidden`, collection `visible`) is
  editor-only: anything that picks or selects in the viewport uses
  `SceneVisibility` in `Editor` mode, and Play mode renders in `Runtime` mode.
- Automated editor runs must set `ZENITH_EDITOR_SETTINGS_DIR` so they never
  read or replace the user's settings directory. They also set
  `ZENITH_EDITOR_SMOKE`, which keeps the Project Hub away.
- State that must survive a project switch (the ImGui theme and fonts, the
  recent projects) belongs to `EditorApplication`; everything per project
  belongs to `EditorSession`, which is destroyed when the user switches.
  Apply look changes through the application's `EditorAppearance` (the one
  owner of the font cache), never to the ImGui style directly, so the
  Project Hub keeps the look.
- Viewports are multiple-instance areas registered with the session's
  `ViewportRegistry`. Code acting on "the viewport" (view3d operators, modal
  transforms, the status bar) uses `ViewportRegistry::active()` and must
  handle it being null; never reach a viewport through a session member.
  Viewport shading goes to the viewport's render view through
  `setViewSettings`, never into the project's render settings.
- A viewport playing the game (`ViewportPanel::gameView`) belongs to the
  game: it shows the game camera and takes no editor input, so new viewport
  tools must skip it. Player input reaches module systems only through
  `EditorPlayback::gameInput`, and game views hold the mouse only through
  `EditorPlayback::mouseCapturePolicy`.
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
