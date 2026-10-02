#pragma once

#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/operators/KeymapRegion.hpp"
#include "editor/operators/Operator.hpp"
#include "engine/application/Application.hpp"

#include <filesystem>
#include <string>
#include <string_view>

class EditorCamera;
class EditorDiagnostics;

// Session-level workflows a panel can trigger but does not own: running
// operators, the region keymaps, document replacement, and modal transforms.
class EditorSessionActions {
  public:
    // Runs a registered operator with its default properties overridden by a
    // JSON object of property values (empty for none). Failures are reported
    // to diagnostics.
    virtual OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId,
                                          std::string_view properties, KeymapRegion region) = 0;
    // Whether the operator exists and its poll passes now.
    virtual bool operatorAvailable(ApplicationContext& context, std::string_view operatorId) = 0;
    // The bound shortcut for a menu label, or empty.
    virtual std::string operatorShortcut(std::string_view operatorId, std::string_view properties) const = 0;
    // Opens a scene, asking about unsaved changes first.
    virtual void requestOpenScene(ApplicationContext& context, std::filesystem::path path) = 0;
    // Runs the keymap of the region under the mouse.
    virtual void handleObjectKeys(ApplicationContext& context, KeymapRegion region) = 0;
    virtual bool modalTransformActive() const = 0;
    // Advances the active modal transform and draws its overlay in the
    // current window.
    virtual void updateModalTransform(ApplicationContext& context) = 0;

  protected:
    ~EditorSessionActions() = default;
};

// Everything a panel may use during one frame. The session builds it from the
// objects it owns, so it holds references only and must not outlive the call
// it was built for.
struct EditorContext {
    ApplicationContext& app;
    SceneEditing editing;
    Selection& selection;
    EditorDiagnostics& diagnostics;
    EditorCamera& camera;
    EditorMode mode;
    EditorSessionActions& actions;

    bool editMode() const {
        return mode == EditorMode::Edit;
    }

    // Runs an operator as a button or menu item does.
    OperatorResult invoke(std::string_view operatorId, std::string_view properties = {}) {
        return actions.invokeOperator(app, operatorId, properties, KeymapRegion::Global);
    }

    bool canInvoke(std::string_view operatorId) {
        return actions.operatorAvailable(app, operatorId);
    }

    std::string shortcut(std::string_view operatorId, std::string_view properties = {}) const {
        return actions.operatorShortcut(operatorId, properties);
    }

    // Places a model asset as an undoable edit; models can only be placed in
    // Edit mode.
    void placeModel(const std::filesystem::path& path, Entity parent);
};
