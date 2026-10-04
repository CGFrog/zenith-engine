#pragma once

#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/operators/KeymapRegion.hpp"
#include "editor/operators/Operator.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/project/AssetRelocation.hpp"
#include "engine/application/Application.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

class ComponentEditorOverrides;
class EditorDiagnostics;
class EntityEyedropper;
class EditorMenus;
class NavigationWorld;

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
    // Project files moved (in the asset browser or outside the editor): the
    // open document follows its file, and the edited scene's asset references
    // follow the assets.
    virtual void projectFilesMoved(ApplicationContext& context, std::span<const ProjectFileMove> moves) = 0;
    // Project files were moved to the trash or deleted: an open document
    // among them becomes untitled with unsaved changes.
    virtual void projectFilesRemoved(ApplicationContext& context, std::span<const std::filesystem::path> removed) = 0;
    // Runs the keymap of the region under the mouse.
    virtual void handleObjectKeys(ApplicationContext& context, KeymapRegion region) = 0;
    // True while a modal transform runs and for the rest of the frame that
    // ended it, so the ending click reaches no other viewport.
    virtual bool modalTransformActive() const = 0;
    // Every viewport calls this while a modal transform is active; only the
    // viewport the transform started in advances it and draws its overlay in
    // the current window.
    virtual void updateModalTransform(ApplicationContext& context, ViewportId viewport) = 0;

    // Runs the region's mouse bindings (operators/KeymapDispatch): presses
    // this frame of the buttons whose bit (1 << ImGuiMouseButton) is set in
    // allowedButtons, and the Hold, Drag, Release, and Click bindings of a
    // press followed from earlier frames. Call every frame, with no buttons
    // allowed when presses must not count. Returns the bits of the pressed
    // buttons a binding claimed. Without a keymap (the default) nothing is
    // bound.
    virtual uint32_t dispatchMouseChords(ApplicationContext&, KeymapRegion, uint32_t) {
        return 0;
    }

    // Whether a binding in region uses mouse button (an ImGuiMouseButton) with
    // the modifiers held now, so pressing it is not camera navigation.
    virtual bool mouseButtonBound(KeymapRegion, int) const {
        return false;
    }

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
    EditorMode mode;
    EditorSessionActions& actions;
    // The session's entity eyedropper; null where there is none (some tests).
    EntityEyedropper* eyedropper{nullptr};
    // The session's menus, to draw one inline; open a popup menu by running
    // wm.call_menu.
    EditorMenus& menus;
    // Whether the runtime scene (running or paused) is a Simulate, which keeps
    // the editor's view of it (see EditorPlayMode::simulating).
    bool simulating{false};
    // Custom component editors added by the game module, used by the
    // Inspector and Properties editors; null where there are none (tests).
    const ComponentEditorOverrides* componentEditors{nullptr};
    // The preview navigation mesh the viewports draw; null where there is
    // none (tests).
    const NavigationWorld* navigationPreview{nullptr};

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
    // Instantiates a prefab asset (absolute inside the asset root, or
    // project-relative) under parent, or at the scene root when parent is
    // null, by running prefab.instantiate.
    void placePrefab(const std::filesystem::path& path, Entity parent);

    // Whether the entity eyedropper is picking for a reference field. While
    // it is, an entity click in a viewport or the outliner goes to
    // pickEyedropperEntity instead of changing the selection.
    bool eyedropperArmed() const;
    // Stores entity in the armed field as one undo step (ui.eyedropper_pick).
    void pickEyedropperEntity(SceneEntityId entity);
    // Whether the eyedropper is armed or the right-click that cancelled it is
    // still held; context menus stay closed meanwhile.
    bool eyedropperBlocksContextMenus() const;
};
