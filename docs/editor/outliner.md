# Outliner (`panels/HierarchyPanel`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`panels/HierarchyPanel` is the outliner. Its rows come from the pure model
in `panels/outliner/OutlinerTree` (`buildOutlinerTree`), one tree per display
mode kept as the area's state: **View Layer** (hierarchies grouped by
collection), **Collections** (the collection tree with flat member lists),
**Prefabs** (instance roots grouped under their prefab), and **Orphan Data**
(`outliner/OrphanData`: project models, textures, and audio the scene's
`{guid, path}` references do not name, cached per `CommandHistory::revision`,
and collections nothing is linked to). The search (`outliner/OutlinerFilter`)
keeps every match with its ancestors (shown dimmed and opened). It is
whitespace-separated terms that must all match, each a case-insensitive
substring: plain text matches the name, `c:` / `component:` a component's type
key or display name, `t:` / `tag:` a `TagsComponent` tag, and `p:` / `prefab:`
an instance's prefab path; double quotes keep spaces (`c:"rigid body"`), an
unknown prefix is plain text, and rows that are not entities match name terms
only. The **Filter** button next to the search shows the syntax and runs
**Select All Matching** (`outliner.select_matching`), which the entity menu
offers too while searching. Rows have
stable ids (`HierarchyPanel::entityRowId`, scoped per collection or prefab
group, so an entity listed twice has two rows); Ctrl+click extends the
selection and Shift+click selects the displayed range from the last clicked
row (`Selection::clickRange`, `displayedRange`). The eye, arrow, and camera
restriction columns run `object.set_visibility` for entities and
`collection.set_flag` for collections; dragging a selected row onto an
entity runs `outliner.parent_drop` for the whole selection, and onto a
collection `collection.link` (the drag payload stays one entity id so entity
reference fields still accept it). Context menus add the collection and
prefab operators. Dropping in the top or bottom quarter of a row whose
parent row is an entity runs `outliner.reorder` for the dragged object
instead, placing it before or after that row (a line shows where); the
order is the scene's child order, which scene documents save
([Scene](../engine/scene.md)). Root objects, and members listed under a
collection, stay in id order. A collection renames in place on double-click,
F2 over its row, or its menu's Rename (`collection.rename`): Enter or
clicking away commits, Escape cancels.

The panel scales to large scenes (`outliner/OutlinerRows`): the tree is
built into an `OutlinerRowCache` and rebuilt only when its
`OutlinerRowSource` changes (the history revision, the scene's content
generation, its entity count, the collection definitions, the search, the
display mode, Orphan Data's assets, and, while the running scene is shown,
every 30 frames, since game code changes it without history). The visible
rows are recomputed only when a row opens or closes (the panel keeps open
rows by row id; collections and groups start open, entities closed, and a
search opens everything without changing them), and only rows in view are
drawn with `ImGuiListClipper`, which keeps the pressed or dragged row, an
open context menu's row, and a renamed row drawn when they scroll away.
When the active object changes elsewhere (a viewport pick), its first row's
ancestors open and it scrolls into view. On `zenith_outliner_scale_benchmark`
(10,000 entities, Debug) a frame with nothing changed costs about 1.2 ms,
against about 160 ms to rebuild the tree, which the panel used to do every
frame. `tests/editor/OutlinerTests.cpp` covers each mode, the filter syntax,
the row cache, orphans, the operators (reorder and collection rename with
undo, select matching), and the panel's clicks, toggles, drags onto and
between rows, clipped rows and reveal, and in-place rename.
