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
undo. What module code gets in each mode:

| | Edit | Play and Simulate |
| --- | --- | --- |
| Systems (`SystemContext`) | `logger`, `debugDraw` | `logger`, `debugDraw`, `services` (`GameServices`), `application`; `input` in Play |
| Module operators (`EditorModuleContext`) | scene, assets, selection, properties, `services`: `random`, `prefabs`, `debugDraw`, `assetRoot` | not run |
| Panels and sidebar tabs | read-only scene, selection, `debugDraw`, `invokeOperator` | the same, on the running scene |

`debugDraw` is the session's `RenderRuntime::debugDraw` in every mode
(`GameModuleHost::setDebugDraw`); `GameServices`, which `enterPlayMode` starts
and `stopPlayMode` (or any module unload) stops, hands out the same list. The
operators' `random` streams are the host's (`GameModuleHost::editRandom`),
kept between runs and started over whenever the module loads, reloads, or
unloads; their `PrefabSystem` is built per call from the document's options,
so it knows the module's component types and holds no module code past the
call.

Hot reload (`module.reload` in the Module menu, a change to the library file
that has stayed unchanged for `GameModuleHostOptions::settleTime`, 250 ms,
for builds started outside the editor, or a successful Build that changed it,
at once) waits for Edit mode, a
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
mode only, bindable like any operator, with properties that Adjust Last
Operation edits: `editorOperator` describes them to the editor's reflected
property widgets as `FieldDescriptor`s over a `ModuleOperatorValues` object
and hands the module an `EditorModuleProperties` per run), ImGui panels (area types toggled in
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
`zenith_game_module_host` runs the test module's `editor_test.generate_preview`
operator through the runner and Adjust Last Operation (seeds, every property
type, panel invocation with values, and the reload releasing the preview and
the random streams).
