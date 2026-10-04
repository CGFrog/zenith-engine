#pragma once

#include "engine/module/EngineExport.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;

// The optional editor half of a game module. Besides zenith_module_register
// (engine/module/ModuleApi.hpp), a module may write
//
//   ZENITH_EDITOR_MODULE_REGISTER {
//       registrar.addOperator({.id = "my_game.reset_spinners", .label = "Reset Spinners", .execute = reset});
//   }
//
// which the editor calls after the module loads. A game executable never
// looks for it, so a module's editor extensions cost nothing at runtime.
// docs/EDITOR_EXTENSIONS.md describes every extension point.
//
// A module links the engine, not the editor, so everything here is either
// engine types, standard library types, or calls the editor dispatches
// through a virtual function: a module must not call editor functions
// directly. Standard library types (std::string, std::function) cross the
// boundary by value, which is why the engine and its modules must be built
// with the same compiler and runtime (the module build hash and
// ZenithEngineConfig.cmake enforce it). The editor removes everything a module
// registered before it unloads the module, and the module registers it again
// after a hot reload.

// Bump whenever EditorModuleRegistrar or a type it passes changes, so the
// editor refuses (with a warning) the editor extensions of a module built
// against other editor headers. 1: operators and panels. 2: inspector
// overrides, sidebar tabs, and menus.
#define ZENITH_EDITOR_MODULE_API_VERSION 2
inline constexpr uint32_t editorModuleApiVersion = ZENITH_EDITOR_MODULE_API_VERSION;

inline constexpr const char* editorModuleRegisterSymbol = "zenith_module_register_editor";
inline constexpr const char* editorModuleApiVersionSymbol = "zenith_module_editor_api_version";

// Ids of editor menus a module may add entries to. They are persisted in user
// keymaps, so they never change.
namespace EditorModuleMenus {
// The Add menu (Shift A).
inline constexpr const char* add = "VIEW3D_MT_add";
// The Add Component menu.
inline constexpr const char* addComponent = "OBJECT_MT_add_component";
} // namespace EditorModuleMenus

struct EditorModuleResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// What a module operator may touch while it polls or runs. It holds
// references only, valid for the one call it is built for.
struct EditorModuleContext {
    Scene& scene;
    AssetManager& assets;
    // Stable ids of the selected entities, the active one first. An operator
    // may replace them; the editor selects the result.
    std::vector<SceneEntityId>& selection;
};

// A module command, offered in the F3 search, bindable in the keymap, and
// run by id like the editor's own operators. Module operators run only in
// Edit mode.
struct EditorModuleOperator {
    // Stable id; prefix it with the module's name, for example
    // "my_game.reset_spinners".
    std::string id;
    std::string label;
    std::string description;
    // Records the operator's scene change as one undo step. The operator
    // must then not record history itself (it cannot; this API has none).
    bool undoable{true};
    // Whether the operator can run now. Empty means whenever the editor is
    // in Edit mode.
    std::function<bool(const EditorModuleContext&)> poll;
    std::function<EditorModuleResult(EditorModuleContext&)> execute;
};

// What a module panel or sidebar tab may use while it draws. The scene is
// read-only: a panel changes the document by invoking an operator, so the
// change is undoable.
struct EditorModulePanelContext {
    const Scene& scene;
    const AssetManager& assets;
    std::span<const SceneEntityId> selection;
    bool editMode{true};
    // Runs an operator (the module's or the editor's) by id, as a button
    // does. Returns whether it finished.
    std::function<bool(std::string_view operatorId)> invokeOperator;
};

// A dockable editor window drawn by the module with Dear ImGui (the engine
// shares one ImGui context with modules). The editor opens the window and
// calls draw between ImGui::Begin and ImGui::End.
struct EditorModulePanel {
    // Window title; unique among the module's panels.
    std::string title;
    std::function<void(EditorModulePanelContext&)> draw;
};

// A tab of every 3D viewport's sidebar (N panel), drawn after the built-in
// Item, Tool, and View tabs inside the sidebar's child window.
struct EditorModuleSidebarTab {
    // Tab label; unique among all sidebar tabs, and not Item, Tool, or View.
    std::string title;
    std::function<void(EditorModulePanelContext&)> draw;
};

// One component shown by an inspector override, valid for the one draw call
// it is built for.
struct EditorModuleInspectorContext {
    // Read-only, for looking at other components; edit only component.
    const Scene& scene;
    const AssetManager& assets;
    // The entity whose component is shown: the active one when several
    // selected entities share the component.
    SceneEntityId entity;
    std::string_view componentKey;
    // The component's value, edited in place. It is the C++ type the
    // component type was registered with; read it through get<T>().
    void* component{nullptr};
    bool editMode{true};

    template <typename Component> Component& get() const {
        return *static_cast<Component*>(component);
    }
};

// A custom inspector for one component type, built-in or the module's,
// replacing its reflected fields in the Inspector and Properties editors. The
// draw function edits the component in place with Dear ImGui and returns
// whether it changed it this frame. The editor records the change as one
// undoable step and, when several selected entities share the component,
// copies the top-level document keys the edit changed to the others, so draw
// only ever sees one component.
struct EditorModuleInspectorOverride {
    // The component type key, for example "my_game.spinner" or "point_light".
    std::string componentKey;
    std::function<bool(EditorModuleInspectorContext&)> draw;
};

// A popup menu the module defines, opened with the wm.call_menu operator
// ({"menu": id}) from a key binding or the Module menu, or shown as the
// submenu of an entry in another menu.
struct EditorModuleMenu {
    // Stable id, unique among all menus; prefix it with the module's name in
    // capitals, for example "MY_GAME_MT_tools".
    std::string id;
    std::string title;
};

// An entry of an editor or module menu. Entries are data: they name an
// operator (whose poll enables the entry and whose key binding labels it) or
// another menu as a submenu.
struct EditorModuleMenuEntry {
    // The menu to add to: one of EditorModuleMenus, or a menu the module
    // added before this entry.
    std::string menu;
    std::string label;
    // Exactly one of operatorId and submenu.
    std::string operatorId;
    // A JSON object overriding the operator's default properties, or empty.
    std::string properties;
    std::string submenu;
    // Categories the entry is nested in, outermost first; empty places it at
    // the top level of the menu.
    std::vector<std::string> category;
};

// What zenith_module_register_editor receives. It lives only for the
// duration of the call; a module must not keep a reference to it. Every add
// function returns a descriptive failure for an invalid or duplicate
// registration, which the editor also reports as a warning.
class EditorModuleRegistrar {
  public:
    virtual EditorModuleResult addOperator(EditorModuleOperator op) = 0;
    virtual EditorModuleResult addPanel(EditorModulePanel panel) = 0;
    // The functions below are new in editor module API 2.
    virtual EditorModuleResult addSidebarTab(EditorModuleSidebarTab tab) = 0;
    // Rejects a component key no registered component type uses, or one that
    // already has an override.
    virtual EditorModuleResult addInspectorOverride(EditorModuleInspectorOverride inspector) = 0;
    virtual EditorModuleResult addMenu(EditorModuleMenu menu) = 0;
    virtual EditorModuleResult addMenuEntry(EditorModuleMenuEntry entry) = 0;

  protected:
    ~EditorModuleRegistrar() = default;
};

extern "C" {
using ZenithEditorModuleApiVersionFunction = uint32_t (*)();
using ZenithEditorModuleRegisterFunction = void (*)(EditorModuleRegistrar&);
}

// Defines the module's editor API version and editor registration function;
// follow it with the function body, which receives `registrar`. Use it once,
// at global scope, in one source file of the module.
#define ZENITH_EDITOR_MODULE_REGISTER                                                                                  \
    extern "C" ZENITH_MODULE_EXPORT uint32_t zenith_module_editor_api_version() {                                      \
        return editorModuleApiVersion;                                                                                 \
    }                                                                                                                  \
    extern "C" ZENITH_MODULE_EXPORT void zenith_module_register_editor(EditorModuleRegistrar& registrar)
