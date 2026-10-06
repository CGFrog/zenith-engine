#pragma once

// Play targets, entering and leaving Play and Simulate, game views and the input and Ui they route, the standalone
// game.

#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/EditorSessionCommands.hpp"
#include "editor/application/PlayTargets.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "engine/application/Application.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/ui/GameUi.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class AreaRegistry;
class CommandHistory;
class EditorDiagnostics;
class EditorDocument;
class GameModuleHost;
class Selection;
class WorkspaceSet;
struct NavMeshBuildSettings;

// What EditorPlayback needs of the session it serves. References outlive the
// playback; the session declares it after the module host and binds the
// workspaces and area types, constructed later, by reference only.
struct EditorPlaybackServices {
    EditorPlayMode& playMode;
    EditorDocument& document;
    CommandHistory& history;
    Selection& selection;
    EditorDiagnostics& diagnostics;
    ViewportRegistry& viewports;
    WorkspaceSet& workspaces;
    const AreaRegistry& areaTypes;
    GameModuleHost& moduleHost;
    // The project's settings Play's navigation bakes with.
    const NavMeshBuildSettings& navigationSettings;
    // The open project's file; empty without a project (the Standalone target
    // and Play's saves need one).
    const std::filesystem::path& projectFile;
    // The game executable and process factory for the Standalone target.
    EditorPlayServices play;
    // Told the mouse capture policy every frame; may be empty.
    std::function<void(MouseCapturePolicy)> mouseCaptureChanged;
    // The viewport of the level the game plays in (never a prefab window's),
    // or null.
    std::function<ViewportPanel*()> activeLevelViewport;
    // A floating play viewport opened or closed an area of a workspace.
    std::function<void()> workspacesChanged;
};

// Runs the document as Play or Simulate on a throwaway copy (EditorPlayMode),
// in a play target: the active level viewport, a floating viewport, or a
// standalone game process. Owns the game view's input routing and Ui, the
// gameplay stages' failure handling, and the mouse capture policy. The session
// forwards its ApplicationScene frame hooks here and draws the playback
// toolbar and menus from the accessors.
class EditorPlayback final : public EditorPlayCommands {
  public:
    explicit EditorPlayback(EditorPlaybackServices services);
    EditorPlayback(const EditorPlayback&) = delete;
    EditorPlayback& operator=(const EditorPlayback&) = delete;

    // EditorPlayCommands
    bool standaloneRunning() const override;
    PlayTarget lastPlayTarget() const override;
    OperatorResult startPlay(ApplicationContext& context, PlayTarget target) override;
    void startSimulate(ApplicationContext& context) override;
    bool playActive() const override;
    // Leaves Play or Simulate, or stops the standalone game, and ends the
    // target's game view (closing a floating one).
    void stopPlay(ApplicationContext& context) override;
    ViewportPanel* gameViewport() const override;

    // Runs the document on a throwaway copy as Play or Simulate; false (after
    // reporting why) when it could not.
    bool enter(ApplicationContext& context, EditorRuntime runtime = EditorRuntime::Play);
    // Leaves Play or Simulate, restoring the edit scene; nothing in Edit mode.
    void stop(ApplicationContext& context);

    // The running target, while Play or Simulate runs or the standalone game
    // does.
    const std::optional<PlayTarget>& activeTarget() const;
    const StandalonePlayer& standalone() const;
    const std::filesystem::path& gameExecutable() const;
    // The input module systems get: the frame's input while a game view has
    // focus (or Play runs without one), otherwise none.
    const InputState& gameInput(const InputState& input) const;
    // Persistent while a game view has game input focus and the mouse over
    // it (or the cursor captured), so a click into it captures the cursor
    // until Esc; otherwise WhileRightMouseDown for editor camera flight.
    MouseCapturePolicy mouseCapturePolicy() const;

    // Frame hooks, in the order the session calls them.
    // Streams the standalone game's output and notes when it exits.
    void pollStandalone(ApplicationContext& context);
    // The frame's Update systems and timers while Play or Simulate runs.
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds);
    void fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds);
    void afterPhysicsStep(ApplicationContext& context);
    void lateUpdate(ApplicationContext& context, float deltaSeconds);
    // The editor's clock with the game's own pause and time scale on top.
    SimulationClock simulationClock() const;
    // Between the frame's gameplay and the editor's UI: stops a run whose
    // gameplay failed (reporting why), ends a frame step, and runs the
    // module's Ui systems into the game view.
    void beginUiFrame(ApplicationContext& context);
    // Tells the application the mouse capture policy, once per frame.
    void publishMouseCapture();
    // The session is ending: stops whatever runs and hands the mouse back.
    void unload(ApplicationContext& context);

  private:
    OperatorResult startStandalone(ApplicationContext& context);
    void endGameView();
    // Runs one stage of Play's or Simulate's gameplay (stage names it for
    // the report) unless an earlier stage of the run failed. A stage fails
    // when an exception escapes it or a module system threw and was
    // disabled in it; the failure is kept for stopFailedGameplay.
    void runGameplayStage(std::string_view stage, const std::function<void()>& run);
    // Leaves Play or Simulate, restoring the edit scene, when a stage failed
    // since the last call, and reports why in the Info log. The engine calls
    // the session between stages, so the session stops at the next safe
    // point (drawUi) rather than inside the engine's fixed-step loop.
    void stopFailedGameplay(ApplicationContext& context);
    // Runs the module's Ui systems into m_gameViewUi for the game view.
    void drawGameViewUi(ApplicationContext& context);

    EditorPlaybackServices m_services;
    // The target F5 (play.start without a target) runs: the last one used.
    PlayTarget m_lastPlayTarget{PlayTarget::ActiveViewport};
    // The running target, while Play or Simulate runs or the standalone game
    // does; Simulate always uses its editor cameras (ActiveViewport, with no
    // game view).
    std::optional<PlayTarget> m_activePlayTarget;
    // The viewport showing the game; empty without a game view.
    ViewportId m_gameViewport;
    // The floating target's area and the workspace it was opened in.
    std::string m_floatingPlayArea;
    std::string m_floatingPlayWorkspace;
    // What module systems get while the game view lacks focus.
    InputState m_idleGameInput;
    // The input the frame's Update systems saw, for its LateUpdate and Ui
    // systems; idle outside Play.
    InputState m_frameGameInput;
    // The frame's wall-clock seconds, for the Ui systems.
    float m_frameGameSeconds{0.0f};
    // Why gameplay failed in the current Play or Simulate run, until
    // stopFailedGameplay stops it.
    std::optional<std::string> m_gameplayFailure;
    // What the module's Ui systems draw over the game view, sized to it.
    GameUiContext m_gameViewUi;
    StandalonePlayer m_standalone;
};
