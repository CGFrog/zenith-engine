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
folder. Scenes open on double-click. Prefabs open as documents of their own (prefab mode,
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
fresh thumbnails and an informational note (there is no texture reload API
yet), and moves and removals go to `EditorSessionActions::projectFilesMoved`
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

The catalog lists `.png`, `.jpg`, `.hdr`, and `.ktx2` images as textures and
`.zmat` files as materials. Grid tiles of images show their thumbnails: the
CPU `ThumbnailCache` decodes them on its worker, and `ProjectAssets`'
`ThumbnailTextures` uploads at most 128 of them as renderer UI images
(`RenderRuntime::createUiImage`), destroying the least recently drawn first
and updating one whose thumbnail was decoded again; the session releases
them on unload. Asset field pickers use the same textures
(`AssetBrowserActions::thumbnailTexture`).

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

## Folder contents

`src/editor/project/`: Project tooling: the shared asset service
(`ProjectAssets`: catalog, watchers, change dispatch, favourites), asset
catalog, thumbnails, bakers, the build runner, Build and Package
(`PackageRunner`: the planned release commands and their runner),
compiler-output parsing, IDE launching, the Project Hub (`ProjectHub`), the
recent projects list, and the project template generator (`ProjectTemplate`,
from `templates/`).
