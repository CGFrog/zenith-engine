# Editor extensions

A game module can extend the Zenith editor with its own operators, panels,
viewport sidebar tabs, component inspectors, and menus. These extensions live
in the module's shared library next to its gameplay, are registered when the
editor loads the module, and are removed and registered again on every hot
reload. A game executable never looks for them, so they cost nothing at
runtime.

The whole API is the header `src/editor/modules/EditorModuleApi.hpp`. The
editor's host for it is `GameModuleHost` (`src/editor/modules/`);
`tests/editor/modules/EditorTestModule.cpp` uses every extension point and is
the reference example.

## Registering

Write the registration function once, at global scope, in one source file of
the module:

```cpp
#include "editor/modules/EditorModuleApi.hpp"

#include <imgui.h>

ZENITH_EDITOR_MODULE_REGISTER {
    registrar.addOperator({.id = "my_game.reset_spinners", .label = "Reset Spinners", .execute = resetSpinners});
    registrar.addPanel({.title = "Spinners", .draw = drawSpinnerPanel});
    registrar.addSidebarTab({.title = "Spinners", .draw = drawSpinnerTab});
    registrar.addInspectorOverride({.componentKey = "spinner", .draw = drawSpinnerInspector});
    registrar.addMenu({.id = "MY_GAME_MT_tools", .title = "My Game"});
    registrar.addMenuEntry({.menu = "MY_GAME_MT_tools", .label = "Reset Spinners",
                            .operatorId = "my_game.reset_spinners"});
    registrar.addMenuEntry({.menu = EditorModuleMenus::add, .label = "My Game", .submenu = "MY_GAME_MT_tools"});
}
```

`ZENITH_EDITOR_MODULE_REGISTER` defines two C-linkage exports:
`zenith_module_register_editor`, the function body you write, which receives
`registrar`; and `zenith_module_editor_api_version`, which reports the
`ZENITH_EDITOR_MODULE_API_VERSION` the module was compiled with.

Every `add` function returns an `EditorModuleResult`. A rejected registration
(an empty or duplicate id, title, or key; a menu entry naming a menu that does
not exist yet) does not stop the others: the editor reports each one as a
warning under the **Game Module** diagnostics source. Register a menu before
the entries that name it.

## Extension points

### Operators

`addOperator(EditorModuleOperator{id, label, description, undoable, poll,
execute})` adds a command that behaves like the editor's own: it appears in
the F3 search, can be bound in the keymap editor, shows up in Adjust Last
Operation, and can be named by menu entries
and by `invokeOperator` in panels. Prefix the id with the module's name
(`my_game.reset_spinners`); the id is what users' keymaps save.

- Module operators run in Edit mode only.
- `execute` receives an `EditorModuleContext`: the scene, the assets, and the
  selection (active entity first). Replace `selection` to change what is
  selected afterwards.
- With `undoable` (the default) the editor records the scene change as one
  undo step; the operator never touches history itself.
- `poll` (optional) decides whether the operator is available now; it also
  enables or greys out menu entries naming the operator.

Module operators have no default key binding: users bind them in the keymap
editor, and a module offers them through menu entries and panel buttons.

### Panels

`addPanel(EditorModulePanel{title, draw})` adds a dockable window drawn with
Dear ImGui (the engine exports ImGui and shares its context with modules).
Each panel becomes a single-instance area type with id `module:<title>`
(`.` and `#` become `_`), which the **Module** menu toggles and workspaces
save like any other area. A panel open when the module reloads stays open.

`draw` receives an `EditorModulePanelContext`: the scene and assets
(read-only), the selection, whether the editor is in Edit mode, and
`invokeOperator(id)`. A panel changes the document by invoking an operator,
so every change is undoable.

### Viewport sidebar tabs

`addSidebarTab(EditorModuleSidebarTab{title, draw})` adds a tab to the
sidebar (N panel) of every 3D viewport, after the built-in Item, Tool, and
View tabs. The title must be unique among all sidebar tabs. `draw` receives the
same `EditorModulePanelContext` as a panel and is called once per open
viewport that shows the tab.

### Inspector overrides

`addInspectorOverride(EditorModuleInspectorOverride{componentKey, draw})`
replaces how the Inspector and Properties editors show one component type,
built-in or the module's own: the override is drawn instead of the reflected
fields (and instead of any built-in custom editor). Prefer better field
presentation hints in the component's `Fields<T>` description; override only
when reflected fields cannot express the UI.

`draw` receives an `EditorModuleInspectorContext` for one component and
returns `true` in the frame it changed it:

```cpp
bool drawSpinnerInspector(EditorModuleInspectorContext& context) {
    Spinner& spinner = context.get<Spinner>();
    bool changed = ImGui::DragFloat("Speed", &spinner.speed, 0.1f);
    if (ImGui::Button("Reverse")) {
        spinner.speed = -spinner.speed;
        changed = true;
    }
    return changed;
}
```

- `get<T>()` returns the component as the C++ type it was registered with.
  `scene` and `assets` are read-only, for looking at the rest of the entity;
  edit only the component.
- The editor records a change as one undo step; consecutive changes while a
  widget is dragged merge into one, as they do for reflected fields.
- With several entities selected the override sees only the active entity's
  component. The editor then compares that component's document form before
  and after the edit and copies only the top-level keys that changed to the
  other selected entities that have the component (all of them while Alt is
  held), so their other differences survive. It shows a "values differ" hint
  above the override when the selected components differ.
- The key must name a registered component type; each type has at most one
  override.

### Menus

Editor menus are registered data (`MenuRegistry`): entries name an operator,
whose poll enables the entry and whose key binding labels it, or another menu
shown as a submenu.

- `addMenu(EditorModuleMenu{id, title})` registers a popup menu. Give it a
  stable id prefixed with the module's name in capitals
  (`MY_GAME_MT_tools`). The **Module** menu lists the module's menus, and
  users can bind `wm.call_menu` with `{"menu": "MY_GAME_MT_tools"}` to a key.
- `addMenuEntry(EditorModuleMenuEntry{menu, label, operatorId, properties,
  submenu, category})` appends an entry to the module's menu or to an editor
  menu: `EditorModuleMenus::add` (the Add menu, Shift A) or
  `EditorModuleMenus::addComponent`. Give exactly one of `operatorId` and
  `submenu`; `properties` is a JSON object overriding the operator's
  defaults; `category` nests the entry under named categories.

Module component types already appear in Add Component and in Add > Component
without any menu entry.

## Lifetime and hot reload

Everything a module registers holds its code, so the editor removes all of it
before the library unloads, on every hot reload, failed reload, project
close, and editor exit. `GameModuleHost` releases, in order: the module's
operators (and Adjust Last Operation's memory of one), its panels, sidebar
tabs, inspector overrides, and menus (everything registered under the
module's `MenuOwner`), then its systems, the document's component registry,
and the scene's EnTT registry. After the new library loads, the editor calls
`zenith_module_register_editor` again; panels that were open reopen.

The rules for module code follow from that:

- Do not keep the `registrar`, a context, a component pointer, or anything
  else the editor hands you beyond the call it was given for.
- Do not keep editor state in globals that must survive a reload; the reload
  loads a fresh copy of the library. Scene data survives (it is serialized
  and restored), so keep persistent state in components.
- Never call editor functions directly: a module links the engine, not the
  editor. Everything editor-side is reached through the registrar's virtual
  functions and the contexts' members.

## Compatibility

The API passes standard library types (`std::string`, `std::function`,
`std::vector`) across the library boundary by value, and the editor frees what
a module allocates. The editor and the module must therefore be built with the
same compiler, standard library settings, and C runtime. The module build
hash (`engine/module/ModuleApi.hpp`) refuses a module built otherwise, and a
generated project's `ZenithEngineConfig.cmake` refuses to configure with
another compiler.

`ZENITH_EDITOR_MODULE_API_VERSION` is bumped whenever `EditorModuleRegistrar`
or a type it passes changes. When a module reports another version (or none,
which means version 1), the editor loads its gameplay but not its editor
extensions, and warns that the module needs a rebuild.

| Version | Adds |
| --- | --- |
| 1 | Operators and panels. |
| 2 | Viewport sidebar tabs, inspector overrides, and menus; `zenith_module_editor_api_version`. |

## Testing

`zenith_game_module_host_tests` hosts `zenith_editor_test_module` without a
window: it runs the module's menu entry, draws its inspector override with
two entities selected (checking the multi-object copy and undo), draws its
sidebar tab, and checks that every extension is gone after a failed reload or
unload and registered exactly once after each reload.
`zenith_editor_module_smoke` runs the editor smoke on a project naming the
same module and checks that the extensions are in place before and after its
hot reload.
