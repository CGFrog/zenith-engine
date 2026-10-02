#pragma once

#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/interaction/ObjectKeymap.hpp"
#include "editor/interaction/Selection.hpp"
#include "engine/application/Application.hpp"

#include <filesystem>

class EditorCamera;
class EditorDiagnostics;

// Session-level workflows a panel can trigger but does not own: document
// replacement, the object keymap, and modal transforms. The session implements
// this until those workflows become registered operators.
class EditorSessionActions {
  public:
    // Opens a scene, asking about unsaved changes first.
    virtual void requestOpenScene(ApplicationContext& context, std::filesystem::path path) = 0;
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

    // Places a model asset as an undoable edit; models can only be placed in
    // Edit mode.
    void placeModel(const std::filesystem::path& path, Entity parent);
};
