# Outliner (`panels/HierarchyPanel`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`panels/HierarchyPanel` is the outliner. Its rows come from the pure model
in `panels/outliner/OutlinerTree` (`buildOutlinerTree`), one tree per display
mode kept as the area's state: **View Layer** (hierarchies grouped by
collection), **Collections** (the collection tree with flat member lists),
**Prefabs** (instance roots grouped under their prefab), and **Orphan Data**
(`outliner/OrphanData`: project models, textures, and audio the scene's
`{guid, path}` references do not name, cached per `CommandHistory::revision`,
and collections nothing is linked to). The search is case-insensitive and
keeps every match with its ancestors (shown dimmed and opened). Rows have
stable ids (`HierarchyPanel::entityRowId`, scoped per collection or prefab
group, so an entity listed twice has two rows); Ctrl+click extends the
selection and Shift+click selects the displayed range from the last clicked
row (`Selection::clickRange`, `displayedRange`). The eye, arrow, and camera
restriction columns run `object.set_visibility` for entities and
`collection.set_flag` for collections; dragging a selected row onto an
entity runs `outliner.parent_drop` for the whole selection, and onto a
collection `collection.link` (the drag payload stays one entity id so entity
reference fields still accept it). Context menus add the collection and
prefab operators. `tests/editor/OutlinerTests.cpp` covers each mode, the
filter, orphans, the operators, and the panel's clicks, toggles, and drags.
