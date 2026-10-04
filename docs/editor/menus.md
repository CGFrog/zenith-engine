# Menus (`src/editor/menus/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Menus are data. `MenuRegistry` holds named menus (`MenuDefinition`: id such as
`VIEW3D_MT_add`, title, and a submenu or headed-section layout) and their
entries (`MenuEntry`: label, operator id with JSON properties, optional icon,
optional category path, or another menu as a submenu), plus dynamic sections
whose `MenuSectionFill` callback adds entries each time the menu is shown.
Every menu, entry, and section carries a `MenuOwner` token; the editor uses
`editorMenuOwner` and other clients take one from `createOwner` and call
`removeOwner` before their code unloads. `resolve` builds a `ResolvedMenu`
snapshot (copies only, so nothing outlives a module), and `searchMenu` scores
its leaves with `fuzzyMatchScore`. `MenuPopup` draws a snapshot Blender style
(search field focused on open, typed queries list matching runnable leaves as
"Light > Sun"), and `drawMenuItems` draws one inside any ImGui menu (the main
menu's **Edit > Add**). `EditorMenus` is the session-owned registry plus the
open popup, reached through `EditorContext::menus`; `wm.call_menu {menu,
category}` opens a menu at the mouse and `object.add_menu` (Shift A) opens
the Add menu. `PieMenu` draws a menu as a Blender pie in the pixel theme (up
to eight leaves, filled left, right, bottom, top, then the diagonals):
`wm.call_pie {menu}` opens it, it follows the key held at that moment
(letting go picks the item the mouse points at; a quick tap waits for a
click), and Esc or a right click cancels. With the `ui.pie_menus` preference
off (the default) `wm.call_pie` opens the ordinary searchable popup instead.
`DefaultMenus` registers the Add menu (add-object entries plus Component >
category > type running `object.add_empty_with`), the Add Component menu,
whose sections come from the document's component registry in
`AddComponentMenu`, so game module types appear, and the Shading (Z), View
(`), and Pivot Point (.) pies. Tests live in `tests/editor/MenuTests.cpp`.
