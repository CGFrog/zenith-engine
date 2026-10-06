#pragma once

#include "editor/application/SceneEditing.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/operators/KeymapRegion.hpp"
#include "editor/operators/Operator.hpp"
#include "engine/application/Application.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

class PrefabWindowSet;
class EditorDocument;
class EditorMenus;
class EditorPlayMode;
class GameModuleHost;
class InfoLog;
class Selection;
class StandalonePlayer;
class ViewportPanel;
class ViewportRegistry;
class Workspace;
struct LastOperator;

// What the editor smoke steps may use of the editor session, implemented by
// EditorSession for the frame being run. Steps act through operators where
// one exists and read the session's state here; they never reach into the
// session itself.
class EditorSmokeHooks {
  public:
    // Runs a registered operator as a key or menu would; failures are also
    // reported to diagnostics.
    virtual OperatorResult invokeOperator(std::string_view operatorId, std::string_view properties,
                                          KeymapRegion region) = 0;
    virtual OperatorContext operatorContext(KeymapRegion region) = 0;
    // Scene edits as a panel makes them (the running scene's in Play).
    virtual SceneEditing editing() = 0;
    virtual void undo() = 0;
    // Undoes the steps recorded since marker, so a step that took a marker
    // before editing undoes exactly its own steps.
    virtual void undoSince(CommandHistoryMarker marker) = 0;
    // Enters and leaves Play directly, without the play operators; a refusal
    // to enter is reported to diagnostics.
    virtual void enterPlayMode() = 0;
    virtual void stopPlayMode() = 0;

    virtual const CommandHistory& history() const = 0;
    virtual Selection& selection() = 0;
    virtual EditorPlayMode& playMode() = 0;
    virtual EditorDocument& document() = 0;
    // Prefab windows: the open windows, a window's viewport (null when it
    // has none), edits of a window's document as its panels make them,
    // saving it as its Save button does (false after reporting why not), and
    // closing one at the next update without asking about unsaved changes.
    virtual const PrefabWindowSet& prefabWindows() const = 0;
    virtual ViewportPanel* prefabWindowViewport(uint64_t window) = 0;
    virtual std::optional<SceneEditing> prefabWindowEditing(uint64_t window) = 0;
    virtual bool savePrefabWindow(uint64_t window) = 0;
    virtual void closePrefabWindow(uint64_t window) = 0;
    virtual EditorMenus& menus() = 0;
    virtual ViewportRegistry& viewports() = 0;
    virtual Workspace& activeWorkspace() = 0;
    // Restores an ImGui dock layout (from captureImGuiLayout) and saves it as
    // the active workspace's layout.
    virtual void restoreWorkspaceLayout(const std::string& layout) = 0;
    // The engine log and diagnostics the Info area shows.
    virtual const InfoLog& infoLog() const = 0;
    // The project's render settings as the editor requested them.
    virtual const RenderSettings& requestedRenderSettings() const = 0;
    virtual const LastOperator* lastOperator() const = 0;

    virtual const GameModuleHost& moduleHost() const = 0;
    // Hot reloads the game module at the next update that allows it.
    virtual void requestModuleReload() = 0;
    virtual bool moduleReloadPending() const = 0;

    // The viewport playing the game, or null.
    virtual ViewportPanel* gameViewport() = 0;
    // Whether module systems get this frame's input (EditorSession::gameInput).
    virtual bool gameReceivesInput() const = 0;
    virtual MouseCapturePolicy mouseCapturePolicy() const = 0;
    // Whether Play, Simulate, or a standalone game runs, or a play target is
    // still active.
    virtual bool playTargetActive() const = 0;
    virtual const StandalonePlayer& standalone() const = 0;
    // The game executable the Standalone target launches.
    virtual const std::filesystem::path& gameExecutable() const = 0;

  protected:
    ~EditorSmokeHooks() = default;
};

// Everything an editor smoke step gets for one frame; references only, valid
// for that frame.
struct SmokeStepContext {
    ApplicationContext& app;
    EditorSmokeHooks& editor;
};
