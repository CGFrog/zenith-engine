# Areas and workspaces (`src/editor/areas/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Windows are Blender-style areas. `AreaRegistry` holds the editor types an area
can show: a stable id (persisted, so never renamed), a title, a panel factory,
and whether a workspace may show several at once. The built-in types are
listed, with their instancing, in `application/EditorAreaTypes.hpp`
(`builtInAreaTypes`): Hierarchy, Inspector, Properties, Diagnostics, Render
Statistics, Asset Browser, and Graphics are multiple-instance, each area with
its own panel over shared services (`ProjectAssets`, the
`RenderSettingsController`); Build and Info stay single-instance views
(`AreaType::shared`) of panels the session owns, because one `BuildRunner`
owns the build and the session reads Info's merged log. The multiple-instance
Viewport is registered by the session itself (each area owns a render view;
see `ViewportRegistry` below). Each game module panel becomes a
single-instance type `module:<title>` while the module provides it.
`EditorSession::registerAreaTypes` (`application/EditorAreas.cpp`) registers
the Viewport and then every built-in type.

To add an editor type, write an `EditorPanel` in `panels/` and add one entry
to `builtInAreaTypes`: a factory with `AreaInstancing::Multiple` when each
area can have its own panel (pass shared state in through
`EditorAreaServices`), or `AreaType::shared` for a panel the session must own.
The id is persisted, so pick a stable lowercase id without `.` or `#`. Add it
to a default workspace in `areas/DefaultWorkspaces.cpp` if new users should
see it, and bump `currentWorkspaceFileVersion` when the defaults change.
Game modules add types by registering editor panels (see the game module
extension API); they appear under the Module menu.

A `Workspace` (Layout, Lighting, and Debug by default, shown as tabs in the
main menu bar) lists its areas, each with an id `<type>.<n>` and an ImGui
window named `<title>##<type>.<n>`, and keeps its own ImGui dock layout as
`SaveIniSettingsToMemory` text. Every registered type has at least one area in
each workspace; closing the last one only hides it so it reopens in place.
A per-area panel may keep view state with its area (`EditorPanel::areaState`
and `restoreAreaState`, saved as the area's `state` in `workspaces.json`):
Properties keeps its tab this way, so the default Lighting workspace opens one
Properties area on World and another on Render.

The default workspaces (`areas/DefaultWorkspaces.cpp`) are **Layout** (Viewport,
Hierarchy, Inspector and Properties, Asset Browser and Info below), **Lighting**
(Viewport, Hierarchy, Properties on World above Properties on Render and
Graphics), and **Debug** (Viewport, Render Statistics above the System
Profiler, and Info, Diagnostics, and Build below). A fourth workspace, **Prefab** (`prefabWorkspaceName`; Viewport, Hierarchy,
Inspector, and Properties), is not a default tab: `EditorSession` adds it the first
time a prefab is edited (`WorkspaceSet::add`) and switches back to the previous
workspace when the outermost prefab is left (see
[Scene editing](scene-editing.md#prefab-mode-the-document-stack)).
Each default area names a dock slot (left, right, lower
right, bottom, or the central node) that `buildDefaultDockLayout` splits off
the dockspace when the workspace has no saved layout.
The header menu of each area switches its editor type in place (the new type
takes the area's dock node), opens another area of the type, or closes it.
Area changes are the `wm.toggle_panel` (by type id), `wm.area_new`,
`wm.area_change_type`, `wm.area_close`, `wm.workspace_activate`, and
`wm.workspace_reset` operators. Workspace switches are applied between frames,
because ImGui settings may only be replaced outside a frame. `workspaces.json`
in the editor settings directory stores the workspaces, their areas, and
layouts. Its `version` changes whenever the default workspaces do: a file
from an older version is set aside as `workspaces.json.old` and replaced by
the current defaults (with an Info diagnostic), and a newer or invalid one is kept as `workspaces.json.bad`.
When the file is missing, the `panel.*` flags and `layout.version` from an
older `settings.ini` and its `layout.ini` are migrated into the Layout
workspace (window sections renamed to the area windows).

The settings directory is `%LOCALAPPDATA%\Zenith\Editor` (or the platform's
user data directory) unless `ZENITH_EDITOR_SETTINGS_DIR` names another one;
`src/editor/main.cpp` resolves it. The editor smoke tests set it to a
directory under the build tree, emptied by the `zenith_editor_test_settings_reset`
fixture each ctest run, so they start from the default workspaces and never
touch the user's settings.

## Making a panel visible as an area type

A panel becomes visible to users by adding an area type to
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

## Folder contents

`src/editor/areas/`: Blender-style areas and workspaces: the area type
registry, per-workspace areas and their ImGui dock layouts, the default
workspaces, legacy layout migration, and `workspaces.json`.

`src/editor/panels/`: One class per dockable window, drawn from an
`EditorContext`, plus the floating Preferences window; `panels/outliner/` holds
the outliner's display-mode tree model, orphan data, and row widgets.
