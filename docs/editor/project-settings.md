# Project Settings

Part of the [repository guide](../REPOSITORY_GUIDE.md).

**Edit > Project Settings** (`project.settings`) opens
`panels/ProjectSettingsWindow` on the project file's current values
(`loadProjectSettingsValues`). It edits a draft in three tabs:

- **Physics Layers**: the layer names and a triangular collision matrix
  (`PhysicsLayerTable`). Layers are only added and removed at the end, so the
  indices bodies store keep their meaning; Static, Dynamic, and Player cannot
  be renamed or removed.
- **Input Actions**: the project's action file (`InputActionsJson`): actions,
  their type and clamping, and bindings (source, scale, dead zone, and
  modifier keys). **Override...** copies one of the game module's actions in,
  so its bindings can be changed; the project's actions replace the module's
  of the same name.
- **Tags**: the names the inspector offers for `TagsComponent`.

**Apply** returns the draft, and the session (`EditorProjectSettings.cpp`)
saves it with `saveProjectSettingsValues`, which reads the project file again
first so the render settings and other fields saved since are kept, and
writes the actions to the file the project names, or to `input_actions.json`
beside it. The session then applies the values live: it clears the physics
world and sets the new layers (bodies are rebuilt from the scene), updates
the names the inspector offers (`interaction/ProjectNames`, through
`EditorContext::projectNames`), and gives the module host the new action
overrides. Apply is Edit mode only: a running scene keeps the layers it
started with. `EditorRenderSettings::save` likewise reads the project file
before writing, so neither window overwrites the other's settings.

In the inspector, a UInt32 field with a `nameList` hint (the physics layer
fields) is a combo of the project's names (`ReflectedFieldEditorOptions::
nameList`); an index past them shows its number. `TagsComponent` has a
custom editor (`ComponentEditors::drawTags`) that adds the project's tags or
any other name and marks a tag the project does not declare.

`tests/editor/ProjectSettingsTests.cpp` covers saving and loading, the kept
render settings, validation, the window headless, and the inspector's
names.
