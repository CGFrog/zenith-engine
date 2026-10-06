# Asset Browser and project assets

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/editor/panels/AssetBrowserPanel.*` and `src/editor/project/`

The Asset Browser shows the project's asset and scene roots as a folder tree
with favourites, breadcrumbs, a grid or list of the current folder, and search
and type filters. Models drag out as `ZENITH_MODEL_ASSET` payloads; textures,
sounds, and materials drag to inspector asset fields as
`ZENITH_TEXTURE_ASSET`, `ZENITH_AUDIO_ASSET`, and `ZENITH_MATERIAL_ASSET`
payloads holding UTF-8 paths (`panels/AssetDragPayloads.hpp`), and the drag
tooltip says Assign over a field that takes the asset and Move over a
folder. Scenes open on double-click. Prefabs open in windows of their own (prefab windows,
`prefab.edit`) on double-click or with the context menu's **Edit Prefab**
(`AssetBrowserActions::openPrefab`). Scripts (C and C++ sources and headers) open in the
user's IDE on double-click, from the context menu, or with the details' Open
in IDE button, through `module.open_source` and `ide.command` (Preferences >
External IDE). With a project open, the session also lists the game module's
source folder (`src/` beside the project file, `projectSourceDirectory`) as
a third root: `AssetCatalogOptions::sourceRoot` is watched and relisted but
has no asset database, so no `.meta` sidecars are written beside code, its
scripts have no GUID, and scenes cannot be created there. `drawBrowser` draws everything from diagnostics and a
small `AssetBrowserActions` set, so tests drive it in a headless ImGui frame.
The panel holds only view state (current folder, selection, search, filters,
view mode, dialogs) over a shared `project/ProjectAssets` service, so several
browsers can show one project. `ProjectAssets` owns the catalog, the CPU
`ThumbnailCache`, change polling and dispatch, the file operations, and the
settings file. The session calls `pollChanges` once per frame, so each change
on disk is scanned and dispatched once however many browsers are open; renames
and moves made through the service are logged (`recentItemMoves`) so every
browser carries its folder and selection along. Every Asset Browser area has
its own panel. Favourites are shared; only the primary browser
(`AssetBrowserRole::Primary`, tracked by `ProjectAssets::claimPrimaryView`)
remembers its folder and view mode. Area browsers start secondary; the first
one drawn while no browser is primary takes the role, and a primary that is
hidden, closed, or no longer drawn (in another workspace or a background tab)
loses it so the next browser drawn is promoted.
All file-system knowledge stays behind `project/AssetCatalog` (asset records
plus every folder, empty ones included). The catalog owns one `AssetDatabase`
per root (a scene root inside the asset root shares the asset root's) and a
`FileWatcher` per root: `poll()` rescans only the roots whose watcher saw a
change, and reports added, modified, removed, and moved assets plus every
moved or removed file. Roots that cannot be watched fall back to a rescan once
a second. `ProjectAssets` acts on the changes: modified models are reloaded
through `AssetManager::reloadModel` with a diagnostic, modified textures get
fresh thumbnails and are reloaded (see Import settings and reimport below),
modified sounds are reloaded, and moves and removals go to `EditorSessionActions::projectFilesMoved`
and `projectFilesRemoved`. The session then relocates the open document when
its own file moved (or detaches it, untitled and dirty, when it was removed)
and points the edited scene's asset references at moved assets
(`project/AssetRelocation`, which rebuilds only the affected entities). The
asset root's database is shared with `EditorDocument`'s scene options, so
saved scenes record GUIDs. `project/ProjectFileOperations` creates folders and
empty scenes, renames (keeping file extensions) and moves items through the
database so `.meta` sidecars travel and GUIDs persist, duplicates (copied
sidecars get fresh GUIDs and keep their import settings), moves items and
their sidecars to the OS trash (Recycle Bin through `IFileOperation`, or the
XDG home trash; never a silent permanent delete), and reveals items in
Explorer or via `xdg-open`; none of them overwrite an existing item. Any item
drags onto a folder in the tree or contents to move it within its root. `settings/AssetBrowserSettings` persists the view
mode, favourites, and last folder in `asset-browser.json` in the editor's
settings directory, falling back to defaults with a warning when the file is
invalid.

The catalog lists `.png`, `.jpg`, `.hdr`, and `.ktx2` images as textures,
`.zmat` files as materials, and JSON arrays as data tables. Grid tiles of images show their thumbnails: the
CPU `ThumbnailCache` decodes them on its worker, and `ProjectAssets`'
`ThumbnailTextures` uploads at most 128 of them as renderer UI images
(`RenderRuntime::createUiImage`), destroying the least recently drawn first
and updating one whose thumbnail was decoded again; the session releases
them on unload. Asset field pickers use the same textures
(`AssetBrowserActions::thumbnailTexture`).

The folder menu (the **New** button, or a right click on empty space) also
has **Add Prefab**, which writes `New Prefab.prefab` holding one empty root
entity (`createProjectPrefab`), and **New Behaviour...**, **New Component...**,
and **New System...**, which run `module.new_behaviour`, `module.new_component`,
and `module.new_system` through `AssetBrowserActions::runOperator` (enabled by
`canRunOperator`, so they follow the operators' polls); their dialog asks for
the type's name. A new folder, scene, prefab, or material gets a unique default
name and opens the Rename dialog at once (`createAndRename`).

**New Material** in the folder menu writes a default `New Material.zmat`
(`createProjectMaterial`). Selecting a material shows its fields below the
listing, drawn as reflected fields over a `project/MaterialDocument` (texture
slots are asset fields relative to the asset root; GUIDs the asset database
knows locate textures that moved). The document is saved atomically once the
edit ends (no widget active), with texture paths relative to the material's
folder plus their GUIDs, and `AssetBrowserActions::reloadMaterial` reloads it
at once. A material modified on disk reaches `ProjectAssetActions::
reloadMaterial` too; `reloadProjectMaterial` calls
`AssetManager::reloadMaterial` and points every `MaterialOverrideComponent` of
the shown scene (and, in Play, the parked edit scene) that used the previous
material at the new one.

### Importing

Files and folders dropped from Explorer onto an Asset Browser are copied into
the folder under the drop (a tree node or folder tile), else the browser's
current folder; **Import...** (toolbar, and the folder menu) opens the
system's multi-select file dialog (`NativeFileDialog`, SDL's dialog API) for
the current folder. `project/AssetImport` does the copying: a taken name gets
" (1)", " (2)", ... before its extension, nothing existing is replaced, and
`.meta` sidecars, hidden names, and `*.tmp` files stay behind, so a copy from
another project gets its own GUID. `ProjectAssets::importFiles` then rescans,
which writes the copies' sidecars, and the browser selects the last copy.
Imports go only into folders of the asset and scene roots.

The importable types come from the catalog's one extension table
(`editorAssetExtensions`, which `editorAssetKind` also reads;
`importableAssetExtensions` is its non-code entries). The Import button's and
menu item's tooltips list them by kind, and the file dialog offers them as a
"Supported assets" filter beside "All files" (`NativeFileDialog` takes several
`FileDialogFilter`s), so a glTF's separate `.bin` and image files can still be
chosen.

The engine reports drops in `InputState::droppedFiles`; the session queues a
frame's drops in a `FileDropQueue` (`EditorContext::fileDrops`), each window
claims the ones inside its rectangle while drawing, and drops nothing took
are reported at the next update. A model or prefab dropped on a viewport is
imported into `ProjectAssets::defaultImportFolder` (the primary browser's
remembered asset folder, else the asset root) unless it already lies in the
project, then placed under the drop with `object.place_model` or
`prefab.instantiate` (`viewport/ViewportDrop`); other files dropped there are
imported only.

### Import settings and reimport

Selecting a texture shows its **Usage** (Color, Normal map, or Data; see
`engine/assets/TextureImportSettings`), a data table its **Schema**. Edits
stay in the browser until **Apply**, which stores them in the asset's `.meta`
through `AssetDatabase::setImportSettings` (`ProjectAssets::
setImportSettings`) and queues a reimport; **Revert** drops them. **Reimport**
(the selected-asset pane and the item menu) queues the asset as modified
(`ProjectAssets::queueReimport`), so it goes through the same dispatch as a
change on disk: models reload (`reloadProjectModel`), materials and prefabs as
above, textures through `reloadProjectTexture` and sounds through
`reloadProjectAudio` (`project/AssetReload`). A texture reload re-points the
texture bindings of the shown scene (and, in Play, the parked edit scene) at
the new generation, reloads the material files sampling it, and loads it
again with its import settings' usage where a binding read it with another.
A sound reload stops voices of the old clip and points audio emitters at the
new one. Scenes and scripts have nothing to reimport.

### Find References and the delete guard

**Find References** (item menu) lists every scene, prefab, and material of
the project that references the asset, or anything inside a folder, and how
many entities of the open scene use it, with **Select** to select them;
clicking a file reveals it. `project/AssetReferences` reads scene and prefab
documents for asset reference objects (`{"guid", "path"}`, relative to the
asset root) and legacy path strings under keys ending in `_asset`, a prefab's
nested prefab sources (relative to its folder), and a material's texture
references (relative to its folder); a reference matches by GUID or by path.
`AssetReferenceIndex` (owned by `ProjectAssets`) keeps each file's references
and reads a file again only when the catalog reports a new size or write time.
Files inside a searched folder are not counted, so a folder is not kept in use
by its own contents. Open-scene users come from the serialized document
(`sceneEntitiesReferencing`). The **Move to Trash** confirmation names the
files that still reference the item before it is moved.

### Data tables

A `.json` file under the asset or scene root whose top-level value is an
array (its first non-whitespace byte, after an optional UTF-8 byte order
mark, is `[`) is a definition table, `EditorAssetKind::Data`
(`isDefinitionTableFile`). Other JSON, such as settings objects, is not
listed; project files at the project root (`input_actions.json`) are outside
the asset roots anyway. Selecting one shows its entries read-only
(`panels/DataTableView`): the schema's fields as the first columns, then any
other keys. It is validated with the game module's `DefinitionSchema`
(`ModuleRegistrar::addDefinitionTable`, read from `EditorContext::
definitionSchemas` each frame and never kept), chosen by the import setting
`schema` or the file's stem; without one only the structure is checked (an
array of objects whose `id`s, where present, are unique non-empty strings).
A failure is shown in the pane and reported once as a warning in the Info
log; the table is read again when the file, its schema, or
`ProjectAssets::dataRevision` (bumped by a change on disk or Reimport)
changes.

## Folder contents

`src/editor/project/`: Project tooling: the shared asset service
(`ProjectAssets`: catalog, watchers, change dispatch, favourites), asset
catalog, thumbnails, bakers, the build runner, Build and Package
(`PackageRunner`: the planned release commands and their runner),
compiler-output parsing, IDE launching, the Project Hub (`ProjectHub`), the
recent projects list, and the project template generator (`ProjectTemplate`,
from `templates/`).
