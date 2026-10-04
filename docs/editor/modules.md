# Game module hosting (`src/editor/modules/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

When the project names a `gameModule`, `EditorSession` loads it at startup
through `GameModuleHost` (a failure is reported under the `Game Module`
diagnostics source and the editor continues with the built-in types). The
host gives `EditorDocument` the module's merged component registry, so the
startup scene, Play mode, autosave, copy/paste, undo, and the inspector's Add
Component menu all see module components. Module systems run through a
`SystemScheduler`. In Play mode (pause and frame step apply):
- `Update` runs from `EditorSession::update`;
- `FixedUpdate` from `fixedUpdate`;
- `LateUpdate` from `lateUpdate`;
- `Ui`, while a game view is open, from `EditorSession::drawUi`, into a
  `GameUiContext` sized to the view that `GameUiOverlay` draws over its image.

`RunIn::Edit` systems run every Edit-mode frame, with fixed updates on an
editor-side fixed clock. Edit-mode systems change the scene directly, outside
undo. Systems get the engine's log in every mode. In Play and Simulate they
also get the host's `GameServices`, which `enterPlayMode` starts and
`stopPlayMode` (or any module unload) stops.

Hot reload (`module.reload` in the Module menu, a settled change to the
library file, or a successful Build that changed it) waits for Edit mode, a
finished build, and no modal tool, then serializes the scene, releases every
holder of module code (editor extensions, scheduler, the document's registry,
and the scene's whole EnTT registry), unloads, loads a fresh shadow copy, and
restores the scene by stable entity ids, so selection and undo history
survive. If the new library fails to load, the scene is restored with the
built-in types and module components stay as `PreservedComponentsComponent`
JSON until a later reload succeeds.

A module may also export `zenith_module_register_editor`
(`ZENITH_EDITOR_MODULE_REGISTER` in `modules/EditorModuleApi.hpp`, which also
exports `zenith_module_editor_api_version`) to add operators (undoable, Edit
mode only, bindable like any operator), ImGui panels (area types toggled in
the Module menu), viewport sidebar tabs (`ViewportRegistry::addSidebarTab`),
inspector overrides (custom component editors in
`interaction/ComponentEditorOverrides`, which `EntityPropertiesView` prefers
over reflected fields and its built-in editors), and menus and menu entries
(under the host's own `MenuOwner`; the Module menu lists the module's menus).
The API is header-only and dispatches through a virtual registrar, because a
module links the engine but never the editor; the host refuses the editor
extensions of a module reporting another editor API version, and removes
everything a module registered before unloading it. `docs/EDITOR_EXTENSIONS.md`
is the module author's guide. The status bar and Module menu show the loaded
module, its version, and pending or failed reloads; a reload that waits for
Play or Simulate to stop also gets its own status-bar pill.
`zenith_editor_module_smoke` runs the editor smoke on a generated project
naming `zenith_editor_test_module`, hot reloads it once, and checks its
editor extensions are registered before and after the reload.
