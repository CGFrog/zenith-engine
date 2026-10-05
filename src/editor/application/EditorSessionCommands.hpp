#pragma once

// The running session's workflows that its operators start. EditorSession
// implements each interface and hands them to the operator families through
// EditorOperatorServices; the session's own families (declared in
// EditorSessionOperators.hpp) reach the session only through them, never
// through the whole session. Each interface is what one kind of operator
// needs; none of them owns anything.

#include "editor/application/PlayTargets.hpp"
#include "editor/interaction/ModalTransform.hpp"
#include "editor/operators/Operator.hpp"
#include "editor/project/ProjectHub.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct ApplicationContext;
class ViewportPanel;
enum class GeneratedCodeKind : uint8_t;

// File, history, clipboard, and project workflows (file.*, edit.*,
// wm.open_project, wm.project_hub).
class EditorDocumentCommands {
  public:
    // No file dialog is pending and no modal tool runs, in Edit mode.
    virtual bool documentCommandsAvailable(const OperatorContext& context) const = 0;
    // New, Open, and Close ask about unsaved changes first.
    virtual void requestNew(ApplicationContext& context) = 0;
    virtual void requestOpen(ApplicationContext& context) = 0;
    // Saves to the document's file, asking for a path when it has none.
    virtual bool saveDocument(ApplicationContext& context) = 0;
    virtual void requestSaveAs() = 0;
    virtual void undo(ApplicationContext& context) = 0;
    virtual void redo(ApplicationContext& context) = 0;
    virtual OperatorResult copySelection(ApplicationContext& context) = 0;
    // Pastes without recording undo; edit.paste is an Undo operator.
    virtual OperatorResult pasteClipboard(ApplicationContext& context) = 0;
    // The editor runs with a Project Hub and nothing modal is open.
    virtual bool projectHubAvailable() const = 0;
    virtual void requestProjectHub(ApplicationContext& context, ProjectHubEntry entry) = 0;
    virtual void requestClose(ApplicationContext& context) = 0;

  protected:
    ~EditorDocumentCommands() = default;
};

// Starting and stopping Play, Simulate, and the standalone game, and the game
// view's input focus (the play.* operators the session runs).
class EditorPlayCommands {
  public:
    virtual bool standaloneRunning() const = 0;
    // The target play.start runs without one: the last one used.
    virtual PlayTarget lastPlayTarget() const = 0;
    virtual OperatorResult startPlay(ApplicationContext& context, PlayTarget target) = 0;
    virtual void startSimulate(ApplicationContext& context) = 0;
    // Whether Play, Simulate, or a standalone game is running (or paused).
    virtual bool playActive() const = 0;
    virtual void stopPlay(ApplicationContext& context) = 0;
    // The viewport playing the game, or null.
    virtual ViewportPanel* gameViewport() const = 0;

  protected:
    ~EditorPlayCommands() = default;
};

// Prefab mode: a prefab open as a document of its own over the parked scene
// (prefab.edit, prefab.exit_edit; application/DocumentStack.hpp).
class EditorPrefabModeCommands {
  public:
    // Whether a prefab is open as the active document.
    virtual bool prefabEditActive() const = 0;
    // Whether prefab mode can be entered or left now: Edit mode, with no
    // file dialog pending and no modal tool running.
    virtual bool prefabEditAvailable(const OperatorContext& context) const = 0;
    // Opens the prefab at reference (project-relative) as the active
    // document, parking the current one; fails, changing nothing, when it
    // cannot be opened or is already open in the stack.
    virtual OperatorResult editPrefab(ApplicationContext& context, const std::filesystem::path& reference) = 0;
    // Returns to the document under the active prefab, asking about unsaved
    // changes first (Save, Discard, or Cancel).
    virtual void requestLeavePrefabEdit(ApplicationContext& context) = 0;

  protected:
    ~EditorPrefabModeCommands() = default;
};

// Blender object-mode tools that follow the mouse or open a popup.
class EditorObjectModeCommands {
  public:
    // A modal G/R/S transform is running (not counting the frame it ended).
    virtual bool modalTransformRunning() const = 0;
    // Starts a modal transform in the active viewport; false when there is
    // none, nothing to transform, or the scene cannot be captured. before is
    // the snapshot to undo to (taken before the copies a duplicate made), or
    // empty to capture the transformed entities now.
    virtual bool beginModalTransform(ApplicationContext& context, TransformOperation operation, std::string commandName,
                                     std::optional<SceneEditing::EntitySnapshot> before, bool commitOnCancel) = 0;
    virtual void openDeleteMenu() = 0;
    // Opens the rename popup for the selection's active object.
    virtual void openRenamePopup(ApplicationContext& context) = 0;

  protected:
    ~EditorObjectModeCommands() = default;
};

// Session windows and state applied between frames: the F3 search, the
// keymap editor, workspace changes, and the deferred module reload.
class EditorWindowCommands {
  public:
    virtual void openOperatorSearch() = 0;
    virtual void openKeymapEditor() = 0;
    // The active workspace's areas changed, so workspaces.json is saved.
    virtual void workspacesChanged() = 0;
    // Restores the active workspace's default areas and dock layout.
    virtual void resetActiveWorkspace() = 0;
    // Reloads the game module at the next update, so module code that asks
    // for it is not unloaded while it runs.
    virtual void requestModuleReload() = 0;
    // Opens the New Behaviour, Component, or System dialog
    // (module.new_behaviour and the others).
    virtual void openNewCodeDialog(GeneratedCodeKind kind) = 0;
    // Opens Project Settings on the project file's current values, showing
    // the tab named tab ("physics_layers", "input_actions", "tags",
    // "navigation"); empty or unknown keeps the window's last tab.
    virtual void openProjectSettings(ApplicationContext& context, std::string_view tab) = 0;

  protected:
    ~EditorWindowCommands() = default;
};
