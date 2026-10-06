# Editor overview

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`src/editor/main.cpp` is the editor composition root: it opens the project
argument, or starts `EditorApplication` in the Project Hub. `EditorApplication`
owns what outlives one project (the ImGui theme and fonts in an
`EditorAppearance`, the recent projects list, the current `ProjectConfig`) and shows either the hub
(`project/ProjectHub`, drawn as an application scene) or one `EditorSession`.
Switching projects replaces the session: **File > Project Hub** asks about
unsaved changes, the hub scene unloads the session, and choosing a project
creates a new session for it. `project/ProjectTemplate` generates projects
from `templates/` and derives a generated project's build (its `zenith` CMake
preset, which the Build panel runs while the build settings are unchanged);
`project/RecentProjects` persists `recent-projects.json`. `application/` owns the
editor lifecycle, the session coordinator, undoable scene editing
(`SceneEditing`), and shared entity commands; `document/` owns scene file state
and command history; `interaction/` owns camera, picking, selection, gizmos,
and component widgets; `panels/` holds one `EditorPanel` per dockable window;
`settings/` owns persisted editor preferences; and `theme/` owns the editor's
look. The editor depends on the engine, while neither the engine nor demo
depends on editor code.

`EditorSession` owns the panels that hold editor-wide state and builds an
`EditorContext` each frame: the application context, a `SceneEditing` view over
the document and history, the selection, diagnostics, camera, and the session
workflows panels may request.

## Info log (`panels/InfoPanel`)

`panels/InfoPanel` is the unified log view. Its CPU-only `InfoLog` model
merges, by time, the engine log (`ApplicationContext::logger`), editor
diagnostics (sources `Build` and `Game Module` get their own source filter),
and the `OperatorRunner`'s bounded `OperatorHistory` (id, label, status,
message, properties JSON, time). The panel filters by severity, source,
engine log category (the **Categories** dropdown lists every category
received, such as a game's `gameplay.<name>` channels from
`GameServices::log(name)`), and case-insensitive text, auto-scrolls while resting at the end, copies the
selected rows (or every shown row) to the clipboard, and clears the view; clear
also clears editor diagnostics but never the engine log. `EditorDiagnostics`
entries carry wall and steady timestamps and a revision counter so readers
re-read only after a change. The editor smoke run fails when the Info model has
received no engine entries by its sixth frame.

## Folder contents

`src/editor/application/`: Editor application lifecycle (including
`EditorAppearance`, the preferred look shared by the Project Hub and sessions)
and the UI/session coordinator, including Play mode and its targets
(`PlayTargets`: game views, input routing, mouse capture, the standalone game
process; wired in `EditorPlayback.cpp`). `EditorSession` is one class
implemented across several files, one per domain: `EditorSession.cpp` (the
frame loop, operators and keymaps), `EditorFileDialogs.cpp`,
`EditorSessionRecovery.cpp` (autosave), `EditorMainMenu.cpp`, `EditorStatusBar.cpp`,
`EditorAreas.cpp`, `EditorObjectCommands.cpp`,
`EditorModuleCommands.cpp`, `EditorPreferences.cpp`,
`EditorProjectSettings.cpp`, `EditorViewports.cpp` and `EditorSessionSmoke.cpp`.
The header tags each member group with its file.
The session owns classes of its own for larger workflows, and forwards its
frame hooks to them: `EditorPlayback` (Play, Simulate, and their targets),
`PrefabWindowController` (prefabs in windows of their own, see
[Prefab windows](prefab-windows.md)), and `GameModuleController` (the game
module's host, loading it, and its hot reloads, see [Modules](modules.md)).
