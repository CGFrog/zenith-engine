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
execute, properties})` adds a command that behaves like the editor's own: it
appears in the F3 search, can be bound in the keymap editor, shows up in
Adjust Last Operation, and can be named by menu entries
and by `invokeOperator` in panels. Prefix the id with the module's name
(`my_game.reset_spinners`); the id is what users' keymaps save.

- Module operators run in Edit mode only.
- `execute` receives an `EditorModuleContext`: the scene, the assets, the
  selection (active entity first), the run's `properties`, and the edit-mode
  `services`. Replace `selection` to change what is selected afterwards.
- With `undoable` (the default) the editor records the scene change as one
  undo step; the operator never touches history itself. Changes to
  transient entities (below) are never recorded.
- `poll` (optional) decides whether the operator is available now; it also
  enables or greys out menu entries naming the operator. It sees the default
  property values.

#### Properties

`properties` declares the operator's parameters as `EditorModuleProperty`
values: a stable `key` (what key bindings and menu entries save), an optional
`label` and `description` (tooltip), and a `defaultValue` whose type is the
property's: `bool`, `int32_t`, `float`, or `std::string`. A string property
with `options` is an enum whose value is one of the option keys; `minimum`
and `maximum` give int and float widgets a range.

```cpp
registrar.addOperator({.id = "my_game.generate", .label = "Generate Level", .execute = generate,
                       .properties = {{.key = "seed", .defaultValue = int32_t{1}},
                                      {.key = "rooms", .defaultValue = int32_t{8}, .minimum = 1.0, .maximum = 64.0},
                                      {.key = "theme", .defaultValue = std::string("crypt"),
                                       .options = {"crypt", "sewer"}}}});
```

- `context.properties` holds every declared property for the run:
  `getInt("seed")`, `getFloat`, `getBool`, `getString` (also for enums).
  `execute` may `set` a value it resolved (keeping its type); Adjust Last
  Operation then shows it.
- An operator with properties is remembered for **Adjust Last Operation**,
  which shows them as the editor's own property widgets and, after an edit,
  undoes the operator's step (when it recorded one) and runs it again with
  the new values.
- F3 search and menu entries without `properties` run it with the defaults;
  a menu entry's or key binding's `properties` JSON (`{"seed": 7}`) and a
  panel's `invokeOperator(id, values)` override the properties they name.
- A registration with an empty or repeated key, or an enum whose default is
  not an option, is rejected with a warning.

#### Edit-mode services

`context.services` is an `EditorModuleServices`, the edit-mode counterpart of
`GameServices` (which exists only in Play and Simulate):

- `random`: `RandomStreams` (`engine/gameplay/Random.hpp`) the editor keeps for
  the module between runs. Reseed them from a seed property so each run is
  deterministic. They start over (seed 0) whenever the module loads, reloads,
  or unloads.
- `prefabs`: a `PrefabSystem` built for the call with the document's options,
  so prefabs holding the module's components instantiate correctly.
- `debugDraw`: the editor's `DebugDraw`, shown in every viewport and game
  view. Use a named channel and `persistent()` for a preview, and clear the
  channel when it regenerates (one-frame primitives disappear after the next
  frame).
- `assetRoot`: the project's asset root, against which prefab and other asset
  references resolve.

`RunIn::Edit` systems get `SystemContext::debugDraw` too (still no
`services`); the table in [Game module hosting](editor/modules.md) lists what
module code gets in each mode.

Module operators have no default key binding: users bind them in the keymap
editor, and a module offers them through menu entries and panel buttons.

### Panels

`addPanel(EditorModulePanel{title, draw})` adds a dockable window drawn with
Dear ImGui (the engine exports ImGui and shares its context with modules).
Each panel becomes a single-instance area type with id `module:<title>`
(`.` and `#` become `_`), which the **Module** menu toggles and workspaces
save like any other area. A panel open when the module reloads stays open.

`draw` receives an `EditorModulePanelContext`: the scene and assets
(read-only), the selection, whether the editor is in Edit mode, the editor's
`debugDraw` (for overlays while the panel is open), and `invokeOperator(id)`
or `invokeOperator(id, values)` with an `EditorModuleProperties` overriding
the named properties. A panel changes the document by invoking an operator,
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

## Recipe: preview a generator for a seed

Transient entities let a generator build its output into the edited scene
without making it part of the document. Tag the root of what you generate
with `Scene::markTransient`; it and its whole subtree are then left out of
saves, autosaves, undo history, copies, and Play (where the game's own
generator builds its level). The Hierarchy dims them with a "generated, not
saved" tooltip. A saved entity's reference to a transient one is saved as
none, with a warning in Info. Rebuilding the scene from its document (a
paste or a module reload) drops the preview; run the operator again.

A "Generate (seed)" operator (the test module's
`editor_test.generate_preview` is a complete example):

```cpp
EditorModuleResult generate(EditorModuleContext& context) {
    // 1. Clear the previous preview and its overlay.
    context.scene.destroyTransient();
    context.services.debugDraw.clearChannel("my_game.layout");

    // 2. Seed the streams from the property: the same seed builds the same level.
    context.services.random.reseed(static_cast<uint64_t>(context.properties.getInt("seed")));
    RandomStream& layout = context.services.random.stream("layout");

    // 3. Build under one transient root.
    const Entity root = context.scene.createEntity("Generated Level");
    context.scene.markTransient(root);
    const DebugDrawRecorder overlay = context.services.debugDraw.channel("my_game.layout").persistent();
    for (int32_t index = 0; index < context.properties.getInt("rooms"); ++index) {
        const glm::vec3 position{layout.range(-20.0f, 20.0f), 0.0f, layout.range(-20.0f, 20.0f)};
        PrefabInstanceResult room =
            context.services.prefabs.instantiate("prefabs/room.prefab", context.scene, root);
        if (!room) {
            return {.error = room.error};
        }
        context.scene.setPosition(room.root, position);
        overlay.aabb(position - glm::vec3(4.0f), position + glm::vec3(4.0f));
        overlay.label(position, "room " + std::to_string(index));
    }
    return {.success = true};
}
```

Register it with a `seed` property (above). Run it from F3, a menu entry, or
a panel button; then change **Seed** in Adjust Last Operation to look at
seed after seed. Because the operator only touches transient entities it
records no undo step, so each re-run simply replaces the preview. The
viewport's **Overlays** menu lists `my_game.layout` under **Debug Draw** to
hide the overlay; a persistent channel stays until the operator clears it.

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
  and restored), so keep persistent state in components. Transient entities
  are not serialized, so a reload drops a generated preview, and the
  operators' random streams start over.
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
| 3 | Operator properties (`EditorModuleProperty`, `EditorModuleProperties`), edit-mode services (`EditorModuleServices`), and the panels' `debugDraw` and `invokeOperator(id, values)`. |

## Testing

`zenith_game_module_host_tests` hosts `zenith_editor_test_module` without a
window: it runs the module's menu entry, draws its inspector override with
two entities selected (checking the multi-object copy and undo), draws its
sidebar tab, and checks that every extension is gone after a failed reload or
unload and registered exactly once after each reload. It also runs the
module's `editor_test.generate_preview` with seeds and every property type,
adjusts it in Adjust Last Operation (the same seed builds the same preview),
invokes it from a panel with values, and checks that a reload drops the
preview and starts the random streams over.
`zenith_editor_module_smoke` runs the editor smoke on a project naming the
same module and checks that the extensions are in place before and after its
hot reload.
