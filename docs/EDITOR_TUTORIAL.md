# Author a first Zenith level

Build the editor from the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel 4
build\Debug\zenith_editor.exe
```

The editor opens the default project when no project file is supplied. To use
another project, pass its `.zenith-project` file as the first argument. Project
asset and scene roots are relative to that file.

1. Choose **File > New** and use the Hierarchy panel's **Create Entity** button.
2. Rename the entity, then edit its position, rotation, and scale in the
   Inspector or with the viewport gizmo (`W`, `E`, and `R`).
3. Open the Asset Browser, find a model, and drag it into the viewport or onto
   an entity in the Inspector. Import failures appear in Diagnostics with a
   suggested next action.
4. The **Properties** panel (docked as a tab beside the Inspector; toggle it in
   **View**) shows the same data Blender-style: pick a tab on its left edge.
   **Object** holds the name and transform, each component category the
   selected entity has (Lights, Physics, a game module's Gameplay, ...) gets
   its own tab with an **Add Component** button for that category, **World**
   edits the scene's sun, environment, fog, and irradiance volumes without
   selecting them (and adds a missing sun, environment, or fog), **Render**
   edits the same render settings as the Graphics panel, and **Scene**
   summarizes the open document. Edits there undo like Inspector edits.
5. Right-click a component's header (or press its **...** button) for
   **Remove**, **Reset to Default**, **Copy Values** / **Paste Values**
   (the values go to the clipboard as JSON), **Copy to Selected** (gives the
   other selected entities the active entity's component and values), and
   **Move Up/Down/to Top/to Bottom**, which reorders that component's section
   for every entity. Collapsed sections and the order are remembered in
   `component-inspector.json` in the editor settings folder. With several
   entities selected, the Inspector and the Properties category tabs list the
   components they all share: a value that differs shows `—`, and an edit sets
   that field on every selected entity as one undo step, leaving their other
   fields alone. Components only the active entity has follow under **Active
   Entity Only**; hold `Alt` while editing one of them to apply the change to
   every selected entity that has the component.
6. Press `Shift+A` over the viewport (or open **Edit > Add**) for the Add
   menu: a cube, sun, environment, fog, or empty, or under **Component** an
   empty entity with one component of any type (a game module's too), named
   after it. Start typing to search the menu (`sun` finds **Light > Sun**),
   then use the arrow keys and `Enter`. **Add Component** buttons open the
   same kind of searchable menu, grouped by category.
7. Use `Shift+D` to duplicate a selection, or `Ctrl+C` and `Ctrl+V` to copy and
   paste serialized entity hierarchies. `Ctrl+Z` undoes; `Ctrl+Shift+Z` or
   `Ctrl+Y` redoes. After adding or duplicating an object, the **Adjust Last
   Operation** panel at the bottom-left of the viewport lets you change its
   location, size, or offset; the operation is redone with the new values.
8. Choose **File > Save As** and save beneath the project's scene root. Saves
   are atomic. A separate recovery file is written periodically while the
   document is dirty and is offered after an interrupted session.
9. Press `F5` to enter Play, `F6` to pause, `F7` to step, and `Shift+F5` to
   stop. Play uses a runtime clone; stopping restores the authored scene.

Every menu item and key runs a named operator. Press `F3` to search all
operators that can run right now, with their shortcuts; use the arrow keys and
`Enter`. **Edit > Keymap...** lists every operator: click a shortcut and press
a new key chord to rebind it (conflicts in the same region are refused), `+`
adds a shortcut, and **Reset** restores the defaults. Your changes are saved to
`keymap.json` in the editor settings folder.

The tabs at the right of the main menu bar are workspaces:

- **Layout** for general editing: the Hierarchy on the left, the Inspector and
  Properties on the right, and the Asset Browser and Info log below the
  viewport.
- **Lighting** for lights and rendering: one Properties area on the **World**
  tab (sun, environment, fog, irradiance volumes) above a second on the
  **Render** tab, beside the Graphics panel's GPU timings.
- **Debug** for investigating problems: Render Statistics beside the viewport,
  and Info, Diagnostics, and Build below it.

Each workspace remembers its own windows and docking. Every window is an area
with a header menu named after its editor type: pick another type to show it
in the same place, **New Area** to open another Hierarchy, Inspector,
Properties, Diagnostics, Render Statistics, Asset Browser, or Graphics beside
it, or **Close Area**. Each area keeps its own view: two Asset Browsers can
show different folders, and each Properties area remembers its tab. The first
Asset Browser you use is the one whose folder is remembered for next time.
**View** shows or hides each editor type in the current workspace, and **View
> Reset Workspace** restores its default layout. Workspaces are saved to
`workspaces.json` in the editor settings folder; the panel layout of older
editor versions is migrated into the Layout workspace the first time, and
workspaces saved by an editor with different default workspaces are replaced
by the new defaults (an Info message says so, and the old file is kept as
`workspaces.json.old`).

The viewport is laid out like Blender's 3D view. Its header holds the
**View** menu (sidebar and toolbar toggles, camera view), **Shading**
(Rendered, Material Preview, Solid, Wireframe; each viewport shades on its own
and never changes the project's render settings; Play mode always shows the
project's look), **Overlays** (grid, selection
outline, active object name, transform gizmo), and **Gizmos** (Global or Local
orientation, snapping and its increments). With the mouse over the viewport,
`T` shows or hides the tool shelf on the left (Tweak, Move, Rotate, Scale) and
`N` shows or hides the sidebar on the right: **Item** edits the active
object's transform, **Tool** the transform tool and snapping, and **View** the
camera's field of view, clip start and end, and fly speed. Clicks on the
header, shelf, or sidebar never select through to the scene.

Open more viewports with an area's **New Area** menu or **View > New Area >
Viewport**; each has its own camera, shading, overlays, and tools. Keys, the
main View menu, the status bar, and `G`/`R`/`S` act on the viewport the mouse
last hovered. `Numpad 0` (or **View > Look Through**) looks through a scene
camera; navigation is locked while you do, and any view key or **View > Exit
Camera View** returns to the editor camera.

The **Overlays** menu also draws the scene's invisible helpers: light ranges
and cones, camera frustums (the active camera highlighted), colliders,
reflection probe, decal, and irradiance volume bounds, selection bounds, and
object names. Each category is Off, Selected Only, or All (colliders and names
start with the selection); hidden objects show none. Overlays stay off while
playing unless **Show in Play Mode** is ticked, and never cover a scene
camera's view. Each viewport remembers its overlays with its area.

Group objects into **collections** the way Blender does: they are separate
from parenting, and an object can be in several. Search (`F3`) for **New
Collection** to create one holding the selection; **Link to Collection** and
**Remove from Collection** change membership, and **Delete Collection** moves
its objects and nested collections to its parent. Each collection can be
hidden in the viewport, made unselectable, or excluded from rendering
altogether (`collection.set_flag`), and **Add Collection Instance** places an
empty that draws a collection's objects at its own transform, offset by the
collection's instance offset; hide the source collection and its instances
still show. Collections are saved with the scene and every change undoes.

With the mouse over the viewport or Hierarchy, `H` hides the selected objects,
`Shift+H` hides everything else, and `Alt+H` reveals and selects every hidden
object. Hiding is an editor aid: hidden objects are saved as hidden, cannot be
clicked or selected with `A`, and still appear in Play mode and in the game.
To leave something out of the game, turn off **Renderable** on its
**Visibility** component or its collection.

New, Open, Exit, and the native window close button ask what to do when the
scene has unsaved changes. **Cancel** always leaves the active document intact.

For contributor checks, run:

```powershell
ctest --test-dir build -C Debug --output-on-failure -j 8
$env:ZENITH_EDITOR_SMOKE='1'; $env:ZENITH_SMOKE_FRAMES='12'; build\Debug\zenith_editor.exe
```

Set `ZENITH_EDITOR_SETTINGS_DIR` to a scratch folder to run the editor without
reading or changing your own workspaces and preferences; the ctest smoke runs
do this automatically.

The second command is the bounded Vulkan smoke scenario for viewport resize
and Edit/Play resource transitions.

To turn an authored scene into a standalone game with movement, weapons,
audio, and tests, continue with
[Build a first-person game with Zenith](tutorials/fps-game/README.md).
