# Editor workspace guide

The editor is a client of `zenith_engine`. Engine and game code must never
depend on this subtree.

## Directory map

| Path | Responsibility |
| --- | --- |
| `main.cpp` | Editor executable composition root. |
| `application/` | Editor application lifecycle and the UI/session coordinator. |
| `areas/` | Blender-style areas and workspaces: the area type registry, per-workspace areas and their ImGui dock layouts, the default workspaces, legacy layout migration, and `workspaces.json`. |
| `document/` | Scene document state, persistence workflow, and undo/redo history. |
| `modules/` | Game module hosting: loading the project's module, running its systems, hot reload, and the header-only editor extension API modules implement. |
| `interaction/` | Editor camera, picking, selection, gizmos, and component editing widgets. |
| `operators/` | Operator registry and runner (poll, execute, undo, Adjust Last Operation), headless object operators, the per-region keymap with `keymap.json` overrides, F3 search, and the keymap editor. |
| `panels/` | One class per dockable window, drawn from an `EditorContext`. |
| `project/` | Project tooling: the shared asset service (`ProjectAssets`: catalog, watchers, change dispatch, favourites), asset catalog, thumbnails, bakers, the build runner, compiler-output parsing, and IDE launching. |
| `settings/` | Persistent editor preferences (legacy panel flags are only read, for migration). |

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
  Properties panels) only when reflected fields cannot express the UI.
- Every user action is a registered `Operator` with a stable id
  (`object.duplicate`, `file.save`). Menus, buttons, and keys invoke it by id
  (`EditorContext::invoke`); add default keys in `operators/DefaultKeymap.cpp`.
  Operators that only edit the document belong in `operators/` and must run
  headless; ones needing the window are registered by `EditorSession`. An
  `Undo` operator must not commit history itself; the runner records it.
- A new window is an `EditorPanel` in `panels/`. Panels receive everything
  through `EditorContext`, record scene changes through `SceneEditing` so they
  are undoable, and never reach into other panels.
- A panel becomes visible to users by registering an area type in
  `EditorSession::registerAreaTypes` (`application/EditorAreas.cpp`) with one
  `addAreaType` call: a panel with per-window state uses a factory and
  `AreaInstancing::Multiple`; a panel the session must own (because other
  workflows use it) is a member shown with `AreaType::shared("id", m_panel)`.
  The type id is persisted in `workspaces.json` and window names, so never
  rename it; add the type to a default workspace in
  `areas/DefaultWorkspaces.cpp` if it should be open for new users. Areas,
  workspaces, and layouts are user data, never scene data.
- Viewports are multiple-instance areas registered with the session's
  `ViewportRegistry`. Code acting on "the viewport" (view3d operators, modal
  transforms, the status bar) uses `ViewportRegistry::active()` and must
  handle it being null; never reach a viewport through a session member.
  Viewport shading goes to the viewport's render view through
  `setViewSettings`, never into the project's render settings.
- Never cache a game module's `ComponentType` pointers or registries across
  frames: read them from `EditorDocument::options()`. Before a module unloads,
  `GameModuleHost` releases its registries, systems, operators, and panels and
  replaces (never clears) the scene registry; a new holder of module code must
  be released there too.
