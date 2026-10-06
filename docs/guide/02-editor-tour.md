# 2. The editor at a glance

[Guide contents](README.md) · Previous: [Getting started](01-getting-started.md) · Next: [Building a level](03-building-levels.md)

Zenith's editor is modelled on Blender: every window is an *area* you can
switch to another editor type, layouts are grouped into *workspaces*, and
every menu item and key runs a named *operator* you can search for and
rebind. If you know Blender, most keys will already feel familiar.

> 📷 **Screenshot needed — `GUIDE-02-OVERVIEW`:** *The whole editor in the Layout
> workspace with a project open. Annotate: main menu bar, playback toolbar,
> workspace tabs (top right), Hierarchy (left), Viewport (centre), Inspector
> and Properties tabs (right), Asset Browser and Info (bottom), status bar.*

## Workspaces

The tabs at the right of the main menu bar are workspaces. Each remembers its
own windows and docking.

| Workspace | Use it for | Contents |
| --- | --- | --- |
| **Layout** | Everyday editing | Hierarchy left; Inspector and Properties right; Asset Browser and Info below the viewport. |
| **Lighting** | Lights, sky, and render settings | Properties on the **World** tab above a second Properties on the **Render** tab, beside the Graphics panel's GPU timings. |
| **Debug** | Investigating problems | Render Statistics and the System Profiler beside the viewport; Info, Diagnostics, and Build below. |

**View > Reset Workspace** restores the current workspace's default layout.
Workspaces are saved to `workspaces.json` in the editor settings folder.

## Areas

Every panel is an *area* with a header menu named after its editor type. From
that menu you can:

- pick another type to show in the same place;
- **New Area** to open another Hierarchy, Inspector, Properties, Diagnostics,
  Render Statistics, Asset Browser, Graphics, or Viewport beside it;
- **Close Area**.

Each area keeps its own view state: two Asset Browsers can show different
folders and each Properties area remembers its tab. **View** shows or hides
each editor type in the current workspace.

> 📷 **Screenshot needed — `GUIDE-02-AREAMENU`:** *An area's header menu opened,
> showing the list of editor types plus New Area and Close Area.*

### The panels you will use most

| Panel | What it does | Reference |
| --- | --- | --- |
| **Viewport** | The 3D view: select, move, and place objects; play the game. | [Viewports](../editor/viewport.md) |
| **Hierarchy** (Outliner) | Every object in the scene as a tree; collections, prefabs, and orphan data views. | [Outliner](../editor/outliner.md) |
| **Inspector** | The selected object's components and their fields. | [Inspector](../editor/inspector.md) |
| **Properties** | The same data, Blender-style, in tabs: Object, one tab per component category, World, Render, Scene. | [Inspector](../editor/inspector.md) |
| **Asset Browser** | The project's assets, scenes, and source folders. Drag models into the viewport. | [Asset Browser](../editor/asset-browser.md) |
| **Info** | The log. Filter by severity and by category (`gameplay.weapons`, `game`, ...). | [Editor overview](../editor/overview.md) |
| **Diagnostics** | Problems with a suggested next action: failed imports, compiler errors, missing assets. | [Asset Browser](../editor/asset-browser.md) |
| **Build** | The game module's build output. Click an error to open it in your IDE. | [Build and IDE](../editor/build-ide.md) |
| **Graphics** / **Render Statistics** | Render settings and per-pass CPU/GPU timings. | [Theme and settings](../editor/theme-settings.md) |
| **System Profiler** | Per-system CPU timings while playing, and which systems Simulate runs. | [Play mode](../editor/play-mode.md) |

## Moving around the viewport

Hover the viewport, then:

| Input | Action |
| --- | --- |
| Hold **right mouse** + move | Look around (fly mode). |
| Hold **right mouse** + `W` `A` `S` `D` | Fly forward, left, back, right. `E` goes up and `Q` down. |
| Hold **right mouse** + mouse wheel | Change fly speed. |
| Hold **middle mouse** + move | Orbit around the focus point. |
| `Shift` + **middle mouse** | Pan. |
| `Ctrl` + **middle mouse**, or the mouse wheel | Zoom. |
| `Numpad .` | Frame the selection (**View > Frame Selected**). |
| `Home` | Frame everything (**View > Frame All**). |
| `Numpad 1` / `3` / `7` (with `Ctrl` for the opposite) | Front / right / top views. |
| `Numpad 2` `4` `6` `8` | Orbit the view in steps. |
| `Numpad 9` | View from the opposite side. |
| `Numpad 0` | Look through the scene's camera; any view key returns. |
| `Numpad /` | Local view: show only the selection. |
| `` ` `` (backquote) | View directions menu (or pie). |

The fly speed, field of view, and clip planes are also on the sidebar's
**View** tab.

> 📷 **Screenshot needed — `GUIDE-02-VIEWPORT`:** *A viewport with its header
> menus (View, Shading, Overlays, Gizmos), the `T` tool shelf open on the left,
> the `N` sidebar open on the right on the Item tab, a selected object with
> its gizmo, and the red-and-white 3D cursor visible. Label each part.*

### Viewport chrome

| Part | Key | What it holds |
| --- | --- | --- |
| Header: **View** | | Sidebar and toolbar toggles, camera view, local view. |
| Header: **Shading** | `Z` | Rendered, Material Preview, Solid, Wireframe. Each viewport shades on its own and never changes the project's render settings. |
| Header: **Overlays** | | Grid, selection outline, object names, light ranges, colliders, camera icons, probe and volume bounds, debug-draw channels, navigation mesh. Each category is Off, Selected Only, or All. |
| Header: **Gizmos** | | Global or Local orientation, pivot point, snapping and its increments. |
| Tool shelf (left) | `T` | Tweak, Move, Rotate, Scale. |
| Sidebar (right) | `N` | **Item** (active transform), **Tool** (transform tool and snapping), **View** (lens and fly speed), **Navigation** (bake and preview the navigation mesh), plus tabs your game module adds. |

You can open as many viewports as you like (**View > New Area > Viewport**);
each has its own camera, shading, overlays, and tools. Keys act on the
viewport the mouse last hovered.

## Finding any command: search

Press `F3` (or `Space`) anywhere to search every operator that can run right
now, with its shortcut. Type part of a name, use the arrow keys, press
`Enter`. This is the fastest way to discover the editor: if you know what you
want but not where it is, search for it.

> 📷 **Screenshot needed — `GUIDE-02-SEARCH`:** *The F3 operator search with
> "collection" typed, showing New Collection, Link to Collection, and others
> with their shortcuts.*

## Keys at a glance

| Key | Operator | Action |
| --- | --- | --- |
| `F3` / `Space` | `wm.search_menu` | Search operators. |
| `Shift+A` | `object.add_menu` | Add menu (objects, lights, components). |
| `G` / `R` / `S` | `transform.translate` / `rotate` / `resize` | Move, rotate, scale with the mouse. Type `X`/`Y`/`Z` to constrain, `Shift+Z` to exclude an axis, a number for an exact value. |
| `Shift+D` | `object.duplicate_move` | Duplicate and move. |
| `X` / `Delete` | `object.delete_menu` / `object.delete` | Delete. |
| `A`, `Alt+A`, `A A`, `Ctrl+I` | `object.select_all` | Select all, deselect, deselect (double tap), invert. |
| `B` | `view3d.select_box` | Box select. |
| `Ctrl` + right drag | `view3d.select_lasso` | Lasso select. |
| `Alt` + click | `view3d.select_cycle` | Cycle through objects under the mouse. |
| `]` / `[` | `object.select_hierarchy` | Select children / parent. |
| `Shift+G` | | Select grouped (same mesh, prefab, tag, collection). |
| `H` / `Shift+H` / `Alt+H` | `object.hide` / `isolate` / `unhide` | Hide selected / hide others / reveal. |
| `Alt+G` / `Alt+R` / `Alt+S` | `object.*_clear` | Clear location / rotation / scale. |
| `F2` | `object.rename_popup` | Rename. |
| `Ctrl+C` / `Ctrl+V` | `edit.copy` / `edit.paste` | Copy and paste entity hierarchies (as JSON). |
| `Ctrl+Z` / `Ctrl+Shift+Z` or `Ctrl+Y` | `edit.undo` / `edit.redo` | Undo / redo. |
| `Shift+Tab` | `view3d.toggle_snapping` | Toggle snapping. |
| `Shift` + right click | `view3d.cursor3d_place` | Place the 3D cursor. |
| `.` | | Pivot point menu (or pie). |
| `Ctrl+B` | `build.run` | Build the game module. |
| `F5` / `Alt+F5` | `play.start` / `play.simulate` | Play / Simulate. |
| `F6` / `F7` | `play.toggle_pause` / `play.step` | Pause / step one frame. |
| `Shift+F5` | `play.stop` | Stop. |

## Rebinding keys

**Edit > Keymap...** lists every operator. Click a shortcut and press a new
chord to rebind it (a conflict in the same region is refused); `+` adds a
shortcut; **Reset** restores the defaults. The box beside each shortcut says
*when* it fires: **Press**, **Twice** (double tap), **Hold**, **Release**,
**Click** (released without dragging), or **Drag**. Mouse gestures in the
viewport are shortcuts too and can be rebound the same way. Your keymap is
saved to `keymap.json` in the editor settings folder.

> 📷 **Screenshot needed — `GUIDE-02-KEYMAP`:** *The Keymap editor filtered to
> "play", showing play.start bound to F5 with its trigger dropdown open.*

## Pie menus

`Z` (shading), `` ` `` (view), and `.` (pivot) open as searchable menus by
default. Turn on **Pie menus** in **Edit > Preferences... > Interface** to get
Blender-style pies: hold the key, point at an item, release.

## Preferences

**Edit > Preferences...** applies each change at once:

| Section | Settings |
| --- | --- |
| **Interface** | Accent colour, UI scale (Automatic or 1x–4x), Pie menus. |
| **Keymap** | Where `keymap.json` lives; opens the keymap editor. |
| **External IDE** | The command that opens a file at a line (`{file}`, `{line}`, `{column}` placeholders) and the debugger attach command; **Test** launches it. |
| **Saving** | How often an unsaved scene's recovery copy is written, or off. |
| **Build** | The Build panel's command and directory, and the release folder for Build and Package. |

An invalid value is shown in red and not used until fixed. Each section has
**Reset to Defaults**. Project-wide settings (physics layers, input actions,
tags, navigation) are separate: **Edit > Project Settings...**, covered in
[chapter 6](06-gameplay-services.md#project-settings).

## Main menus

| Menu | Highlights |
| --- | --- |
| **File** | New, Open..., Save, Save As..., Open Project..., Project Hub, Build and Package, Quit. |
| **Edit** | Undo/Redo, Add, Copy/Paste, Duplicate, Delete, selection, Clear transform, Search Operators..., Keymap..., Preferences..., Project Settings... |
| **Object** | Array, Scatter, **Prefab** (create, apply, revert, unpack, edit), **Navigation** (bake, clear, settings). |
| **View** | Frame Selected/All, Viewpoint, New Area, Reset Workspace, show/hide editor types. |
| **Play** | Play, Play in ..., Simulate, Simulate Systems, Step Frame, Stop, Apply Selection to Edit Scene. |
| **Module** | Build, Reload Game Module, New Behaviour/Component/System..., Attach Debugger, Clear Watched Fields, and the panels and menus your module adds. |

Every item is an operator, so anything in a menu can also be found with `F3`
and bound to a key.

[Guide contents](README.md) · Previous: [Getting started](01-getting-started.md) · Next: [Building a level](03-building-levels.md)
