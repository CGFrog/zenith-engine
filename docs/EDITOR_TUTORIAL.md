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
5. Use `Shift+D` to duplicate a selection, or `Ctrl+C` and `Ctrl+V` to copy and
   paste serialized entity hierarchies. `Ctrl+Z` undoes; `Ctrl+Shift+Z` or
   `Ctrl+Y` redoes. After adding or duplicating an object, the **Adjust Last
   Operation** panel at the bottom-left of the viewport lets you change its
   location, size, or offset; the operation is redone with the new values.
6. Choose **File > Save As** and save beneath the project's scene root. Saves
   are atomic. A separate recovery file is written periodically while the
   document is dirty and is offered after an interrupted session.
7. Press `F5` to enter Play, `F6` to pause, `F7` to step, and `Shift+F5` to
   stop. Play uses a runtime clone; stopping restores the authored scene.

Every menu item and key runs a named operator. Press `F3` to search all
operators that can run right now, with their shortcuts; use the arrow keys and
`Enter`. **Edit > Keymap...** lists every operator: click a shortcut and press
a new key chord to rebind it (conflicts in the same region are refused), `+`
adds a shortcut, and **Reset** restores the defaults. Your changes are saved to
`keymap.json` in the editor settings folder.

New, Open, Exit, and the native window close button ask what to do when the
scene has unsaved changes. **Cancel** always leaves the active document intact.

For contributor checks, run:

```powershell
ctest --test-dir build -C Debug --output-on-failure
$env:ZENITH_EDITOR_SMOKE='1'; $env:ZENITH_SMOKE_FRAMES='12'; build\Debug\zenith_editor.exe
```

The second command is the bounded Vulkan smoke scenario for viewport resize
and Edit/Play resource transitions.

To turn an authored scene into a standalone game with movement, weapons,
audio, and tests, continue with
[Build a first-person game with Zenith](tutorials/fps-game/README.md).
