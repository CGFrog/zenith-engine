#pragma once

#include "engine/module/EngineExport.hpp"
#include "engine/scene/Scene.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;

// The optional editor half of a game module. Besides zenith_module_register
// (engine/module/ModuleApi.hpp), a module may export
//
//   ZENITH_EDITOR_MODULE_REGISTER {
//       registrar.addOperator({.id = "my_game.reset_spinners", .label = "Reset Spinners", .execute = reset});
//   }
//
// which the editor calls after the module loads. A game executable never
// looks for it, so a module's editor extensions cost nothing at runtime.
//
// A module links the engine, not the editor, so everything here is either
// engine types, standard library types, or calls the editor dispatches
// through a virtual function: a module must not call editor functions
// directly. The editor removes everything a module registered before it
// unloads the module.

inline constexpr const char* editorModuleRegisterSymbol = "zenith_module_register_editor";

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

// What a module panel may use while it draws. The scene is read-only: a
// panel changes the document by invoking an operator, so the change is
// undoable.
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

// What zenith_module_register_editor receives. It lives only for the
// duration of the call; a module must not keep a reference to it.
class EditorModuleRegistrar {
  public:
    // Returns a descriptive failure for an invalid or duplicate id, label,
    // or title; the editor also reports it.
    virtual EditorModuleResult addOperator(EditorModuleOperator op) = 0;
    virtual EditorModuleResult addPanel(EditorModulePanel panel) = 0;

  protected:
    ~EditorModuleRegistrar() = default;
};

extern "C" {
using ZenithEditorModuleRegisterFunction = void (*)(EditorModuleRegistrar&);
}

// Defines the module's editor registration function; follow it with the
// function body, which receives `registrar`.
#define ZENITH_EDITOR_MODULE_REGISTER                                                                                  \
    extern "C" ZENITH_MODULE_EXPORT void zenith_module_register_editor(EditorModuleRegistrar& registrar)
