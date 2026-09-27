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
4. Use `Ctrl+D` to duplicate a selection, or `Ctrl+C` and `Ctrl+V` to copy and
   paste serialized entity hierarchies. `Ctrl+Z` and `Ctrl+Y` undo and redo.
5. Choose **File > Save As** and save beneath the project's scene root. Saves
   are atomic. A separate recovery file is written periodically while the
   document is dirty and is offered after an interrupted session.
6. Press `F5` to enter Play, `F6` to pause, `F7` to step, and `Shift+F5` to
   stop. Play uses a runtime clone; stopping restores the authored scene.

New, Open, Exit, and the native window close button ask what to do when the
scene has unsaved changes. **Cancel** always leaves the active document intact.

For contributor checks, run:

```powershell
ctest --test-dir build -C Debug --output-on-failure
$env:ZENITH_EDITOR_SMOKE='1'; $env:ZENITH_SMOKE_FRAMES='12'; build\Debug\zenith_editor.exe
```

The second command is the bounded Vulkan smoke scenario for viewport resize
and Edit/Play resource transitions.
