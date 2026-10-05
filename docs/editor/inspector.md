# Inspector and Properties panels

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`interaction/EntityPropertiesView` draws the selection's sections: name, label
filter, transform, every registered component (reflected fields, or a custom
editor registered there for `collider`, `morph_weights`, `irradiance_volume`,
and `animation`), preserved unknown components, asset warnings, the model drop
target, and the irradiance bake button. Panels add the Add Component button
from `menus/AddComponentMenu`, which opens the searchable Add Component menu
grouped by `ComponentType::category()` (empty categories show as "Other").
Every edit is recorded through `SceneEditing` with a coalesce key naming the
edited entities, and component add runs `object.component_add` (active
entity).

A prefab instance's entities show the instance header (`drawPrefabHeader`):
the prefab's name, its override count, Apply, Revert, Unpack, Select Prefab
Asset, and **Edit Prefab**, which opens the prefab as a document of its own
(`prefab.edit {asset}`). In prefab mode the header is hidden for the
document's own root instance (`EditorContext::prefabEdit`), where Apply would
be Save and Revert would discard the edits; nested instances keep theirs, and
their Apply writes the nested prefab as an undoable step of the open prefab's
history.

Components are listed in the display order of
`settings/ComponentInspectorLayout`, which also remembers collapsed sections
per component type and is saved by the session to `component-inspector.json`.

A game module's behaviours are component types in the `Behaviours` category
(`BehaviourOptions::category`), so Add Component lists them there and they
serialize, multi-edit, undo, override in prefabs, edit live in Play, and apply
to the edit scene like any component. Their headers also have **Edit Script**
(`module.edit_script {component}`), which opens the IDE where the module
registered the type (`GameModuleHost::behaviourSource`, from the
`std::source_location` that `ModuleRegistrar::addBehaviour` records), and a
badge from `EditorContext::scripts` (`modules/EditorScriptInfo`, which the
session implements): **Building** while the project builds, **Build failed**
when the loaded code is older than a failed build. A module that failed to
load leaves its components as "(unknown type)" sections, badged **Reload
failed**.
A component header has Remove and a context menu (right click or "...")
running the headless operators in `operators/ComponentOperators`:
`object.component_remove` (in `ObjectOperators`), `object.component_reset`,
`object.component_copy`/`object.component_paste` (the component's document
JSON on a `TextClipboard`; the session uses `application/SystemClipboard`),
`object.component_copy_to_selected`, and `object.component_move`. All act on
every selected entity that has the component except copy (the active entity)
and move (the layout). With several entities selected the view shows the
components they share; `interaction/ReflectedFieldEditor` compares and copies
each field through its JSON form (`FieldJson`), so a differing field shows
`mixedValueText` (ImGui's mixed state for checkboxes) and an edit changes only
that field, or only the edited vector elements, on every entity in one undo
step. Typing a value into a mixed number and pressing Enter (or leaving the
field) applies it even when it equals the active entity's own value. Custom
editors copy only the top-level document keys they changed. The name and the
transform are edited the same way (the Inspector and the Properties Object
tab show them for a multi-selection): differing names show the mixed mark and
typing renames every selected entity, and Position, Rotation, and Scale are
drawn through `ComponentEditors::editableTransformFields` with the mixed
mark per axis, so an edit changes only that axis on every entity.
Components only the active entity has follow under "Active Entity Only";
holding Alt while editing applies the change to every selected entity with the
component.

Entity reference fields draw through `interaction/EntityReferenceField`: a box
naming the referenced entity ("None", its name, or "Missing" in the error
colour), which accepts an entity dragged from the outliner
(`ZENITH_SCENE_ENTITY`), a Clear button, and a Pick button arming the entity
eyedropper. The armed `EntityEyedropper` names its field by entity id,
component key, and field path only (never a `ComponentType` pointer), and
looks it up again on every use. While it is armed, the next entity clicked in
a viewport or the outliner fills the field through `ui.eyedropper_pick`
instead of changing the selection; Esc, a right-click, Play mode, or the
field's disappearance cancels it, and `EditorSession::updateEyedropper` shows
the hint at the mouse. `tests/editor/EntityReferenceFieldTests.cpp` covers
the widget, an outliner-click pick, and undo.

`panels/InspectorPanel` lists those sections in one scrolling column.
`panels/PropertiesPanel` is the Blender-style alternative: a vertical tab strip
with Object (name, transform, Add Component), one tab per component category
the active entity has (module categories included; with several selected it
edits their shared components), World (the
scene's primary sun, environment light, and fog, chosen as the renderer chooses
them, plus every irradiance volume, with Add Sun/Environment/Fog when
missing), Render (the shared render settings), and Scene (document file,
unsaved state, entity count, asset root, and component registry). It
remembers the selected tab and falls back to Object while that tab is
unavailable. Its visibility is `panel.properties` in `settings.ini`.
`tests/editor/PropertiesPanelTests.cpp` drives both panels headlessly through
`tests/editor/HeadlessImGui.hpp`.
