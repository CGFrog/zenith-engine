// The play operators and EditorPlayback: play targets (the active viewport,
// a floating viewport, or a standalone game process), entering and leaving
// Play and Simulate, game views and the input and Ui they route to module
// systems, and the mouse capture policy.

#include "EditorPlayback.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorSessionOperators.hpp"
#include "editor/areas/AreaRegistry.hpp"
#include "editor/areas/Workspace.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "engine/module/GameEvents.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/module/GuardedCall.hpp"

#include <span>
#include <utility>

namespace {
constexpr const char* playDiagnosticSource = "Play";

struct PlayStartProperties {
    // A PlayTargetInfo key; empty runs the last target used.
    std::string target;
};

std::filesystem::path playTemporaryDirectory(const EditorPlayServices& services) {
    if (!services.temporaryDirectory.empty()) {
        return services.temporaryDirectory;
    }
    std::error_code error;
    const std::filesystem::path systemTemporary = std::filesystem::temp_directory_path(error);
    return error ? std::filesystem::path("zenith-play") : systemTemporary / "zenith-play";
}
} // namespace

OperatorRegistrationResult registerPlayControlOperatorFamily(OperatorRegistry& registry,
                                                             const EditorOperatorServices& services) {
    EditorPlayCommands& play = services.playCommands;
    EditorPlayMode& playMode = services.playMode;
    std::string targetKeys;
    for (const PlayTargetInfo& target : playTargets()) {
        targetKeys += targetKeys.empty() ? "" : ", ";
        targetKeys += "\"" + std::string(target.key) + "\" (" + std::string(target.label) + ")";
    }
    std::vector<Operator> operators;
    operators.push_back(
        {.id = "play.start",
         .label = "Play",
         .description = "Run the scene on a copy of the document in a play target: " + targetKeys +
                        "; without a target, the one used last",
         .properties = OperatorProperties::make(PlayStartProperties{}, std::move(Fields<PlayStartProperties>().add(
                                                                           "target", &PlayStartProperties::target))),
         .poll =
             // The session's state, not context's: a prefab
             // window's document is in Edit mode while the
             // level plays.
         [&play](const OperatorContext& context) { return context.app != nullptr && !play.playActive(); },
         .execute =
             [&play, targetKeys](OperatorContext& context, OperatorProperties& properties) {
                 const PlayStartProperties* values = properties.get<PlayStartProperties>();
                 if (values == nullptr) {
                     return OperatorResult::failed("Operator properties have the wrong type");
                 }
                 PlayTarget target = play.lastPlayTarget();
                 if (!values->target.empty()) {
                     const std::optional<PlayTarget> parsed = parsePlayTarget(values->target);
                     if (!parsed) {
                         return OperatorResult::failed("Unknown play target '" + values->target + "'; use " +
                                                       targetKeys);
                     }
                     target = *parsed;
                 }
                 return play.startPlay(*context.app, target);
             }});
    operators.push_back(applicationOperator(
        "play.simulate", "Simulate",
        "Run physics and the systems chosen in the System Profiler on a copy of the document, keeping the editor "
        "camera and tools",
        [&play](const OperatorContext&) { return !play.playActive(); },
        [&play](ApplicationContext& app) { play.startSimulate(app); }));
    operators.push_back(applicationOperator(
        "play.toggle_pause", "Pause / Resume", "Pause or resume Play or Simulate",
        [&playMode](const OperatorContext&) { return playMode.mode() != EditorMode::Edit; },
        [&playMode](ApplicationContext&) {
            if (playMode.running()) {
                playMode.pause();
            } else {
                playMode.resume();
            }
        }));
    operators.push_back(applicationOperator(
        "play.step", "Step Frame",
        "Advance paused Play or Simulate by one frame of the fixed time step: Update systems, animation, and "
        "audio, plus the fixed update that frame runs",
        [&playMode](const OperatorContext&) { return playMode.mode() == EditorMode::Paused; },
        [&playMode](ApplicationContext& app) { playMode.requestFrameStep(app.physics.config().fixedTimeStep); }));
    operators.push_back(applicationOperator(
        "play.stop", "Stop", "Leave Play or Simulate and restore the authored scene, or stop the standalone game",
        [&play](const OperatorContext&) { return play.playActive(); },
        [&play](ApplicationContext& app) { play.stopPlay(app); }));
    operators.push_back({.id = "play.focus_game_view",
                         .label = "Focus Game View",
                         .description = "Give the game view player input; a click in it then captures the cursor",
                         .flags = OperatorFlags::Internal,
                         .poll = [&play](const OperatorContext&) { return play.gameViewport() != nullptr; },
                         .execute =
                             [&play](OperatorContext&, OperatorProperties&) {
                                 play.gameViewport()->setGameInputFocus(true);
                                 return OperatorResult::finished();
                             }});
    operators.push_back({.id = "play.release_game_input",
                         .label = "Release Game Input",
                         .description = "Take player input and the cursor back from the game view",
                         .flags = OperatorFlags::Internal,
                         .poll =
                             [&play](const OperatorContext&) {
                                 const ViewportPanel* viewport = play.gameViewport();
                                 return viewport != nullptr && viewport->gameInputFocus();
                             },
                         .execute =
                             [&play](OperatorContext&, OperatorProperties&) {
                                 play.gameViewport()->setGameInputFocus(false);
                                 return OperatorResult::finished();
                             }});
    return addOperators(registry, std::move(operators));
}

EditorPlayback::EditorPlayback(EditorPlaybackServices services)
    : m_services(std::move(services))
    , m_standalone(m_services.play.makeGameProcess ? m_services.play.makeGameProcess() : makeGameProcess()) {}

const std::optional<PlayTarget>& EditorPlayback::activeTarget() const {
    return m_activePlayTarget;
}

const StandalonePlayer& EditorPlayback::standalone() const {
    return m_standalone;
}

const std::filesystem::path& EditorPlayback::gameExecutable() const {
    return m_services.play.gameExecutable;
}

bool EditorPlayback::standaloneRunning() const {
    return m_standalone.running();
}

PlayTarget EditorPlayback::lastPlayTarget() const {
    return m_lastPlayTarget;
}

void EditorPlayback::startSimulate(ApplicationContext& context) {
    enter(context, EditorRuntime::Simulate);
}

bool EditorPlayback::playActive() const {
    return m_services.playMode.mode() != EditorMode::Edit || m_standalone.running();
}

OperatorResult EditorPlayback::startPlay(ApplicationContext& context, PlayTarget target) {
    m_lastPlayTarget = target;
    if (target == PlayTarget::Standalone) {
        return startStandalone(context);
    }
    // The game plays in a viewport of the level, never a prefab window's.
    ViewportPanel* viewport = m_services.activeLevelViewport ? m_services.activeLevelViewport() : nullptr;
    if (target == PlayTarget::FloatingViewport) {
        Workspace& workspace = m_services.workspaces.active();
        const AreaResult opened = workspace.openNew(m_services.areaTypes, "viewport");
        AreaInstance* area = opened ? workspace.find(opened.areaId) : nullptr;
        viewport = area != nullptr ? dynamic_cast<ViewportPanel*>(area->panel.get()) : nullptr;
        if (viewport == nullptr) {
            return OperatorResult::failed("Could not open a floating viewport: " + opened.error);
        }
        area->dockRequest = 0;
        area->floatRequest = true;
        m_floatingPlayArea = opened.areaId;
        m_floatingPlayWorkspace = workspace.name();
    }
    if (!enter(context)) {
        endGameView();
        return OperatorResult::failed("Play mode could not start");
    }
    m_activePlayTarget = target;
    if (viewport == nullptr) {
        m_services.diagnostics.add(EditorDiagnosticSeverity::Info, playDiagnosticSource,
                                   "No viewport is open, so the game runs without a view.");
        return OperatorResult::finished();
    }
    viewport->beginGameView();
    m_gameViewport = viewport->id();
    m_services.viewports.activate(m_gameViewport);
    return OperatorResult::finished();
}

OperatorResult EditorPlayback::startStandalone(ApplicationContext& context) {
    const StandaloneResult started = m_standalone.start({.gameExecutable = m_services.play.gameExecutable,
                                                         .projectFile = m_services.projectFile,
                                                         .temporaryDirectory = playTemporaryDirectory(m_services.play)},
                                                        context.scene, context.assets, m_services.document.options());
    if (!started) {
        return OperatorResult::failed(started.error);
    }
    m_activePlayTarget = PlayTarget::Standalone;
    std::string commandLine;
    for (const std::string& argument : m_standalone.arguments()) {
        commandLine += (commandLine.empty() ? "" : " ") + argument;
    }
    context.logger.log(LogLevel::Info, StandalonePlayer::logCategory, "Started " + commandLine);
    return OperatorResult::finished();
}

void EditorPlayback::stopPlay(ApplicationContext& context) {
    if (m_standalone.running()) {
        // The session leaves Play once updateStandalone sees the process end.
        m_standalone.stop();
    }
    stop(context);
}

void EditorPlayback::endGameView() {
    if (ViewportPanel* viewport = gameViewport()) {
        viewport->endGameView();
    }
    m_gameViewport = {};
    if (m_activePlayTarget != PlayTarget::Standalone) {
        m_activePlayTarget.reset();
    }
    if (m_floatingPlayArea.empty()) {
        return;
    }
    if (Workspace* workspace = m_services.workspaces.find(m_floatingPlayWorkspace)) {
        // Hides rather than removes it when the user closed every other
        // viewport meanwhile; either way it leaves the screen.
        workspace->close(m_floatingPlayArea);
        if (m_services.workspacesChanged) {
            m_services.workspacesChanged();
        }
    }
    m_floatingPlayArea.clear();
    m_floatingPlayWorkspace.clear();
}

ViewportPanel* EditorPlayback::gameViewport() const {
    ViewportPanel* viewport = m_gameViewport ? m_services.viewports.find(m_gameViewport) : nullptr;
    return viewport != nullptr && viewport->gameView() ? viewport : nullptr;
}

const InputState& EditorPlayback::gameInput(const InputState& input) const {
    if (!m_gameViewport) {
        // Play without a view (no viewport was open) keeps the old routing.
        return input;
    }
    const ViewportPanel* viewport = gameViewport();
    return viewport != nullptr && viewport->gameInputFocus() ? input : m_idleGameInput;
}

MouseCapturePolicy EditorPlayback::mouseCapturePolicy() const {
    const ViewportPanel* viewport = gameViewport();
    const GameServices* services = m_services.moduleHost.gameServices();
    return playMouseCapturePolicy(viewport != nullptr && viewport->gameViewHoldsCursor(),
                                  services != nullptr && services->outside().pointerReleased());
}

void EditorPlayback::publishMouseCapture() {
    // Every frame rather than on change: a session whose replacement failed
    // to load (and reset the policy while unloading) restores its own.
    if (m_services.mouseCaptureChanged) {
        m_services.mouseCaptureChanged(mouseCapturePolicy());
    }
}

void EditorPlayback::pollStandalone(ApplicationContext& context) {
    if (!m_standalone.poll(context.logger)) {
        return;
    }
    if (m_activePlayTarget == PlayTarget::Standalone) {
        m_activePlayTarget.reset();
    }
}

void EditorPlayback::drawGameViewUi(ApplicationContext& context) {
    ViewportPanel* viewport = gameViewport();
    if (viewport == nullptr || m_services.playMode.mode() == EditorMode::Edit || m_services.playMode.simulating()) {
        return;
    }
    // The game sees the pointer relative to its view, as it would its window.
    InputState input = m_frameGameInput;
    input.mousePosition -= viewport->position();
    const glm::vec2 size = glm::max(viewport->size(), glm::vec2(1.0f));
    m_gameViewUi.beginFrame({static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)}, input);
    runGameplayStage(systemPhaseName(SystemPhase::Ui), [&] {
        m_services.moduleHost.runSystems(SystemPhase::Ui, context.scene, &context, m_services.playMode.simulationMode(),
                                         m_frameGameSeconds, &input, &m_gameViewUi);
    });
    viewport->setGameUi(&m_gameViewUi);
}

void EditorPlayback::runGameplayStage(std::string_view stage, const std::function<void()>& run) {
    if (m_gameplayFailure) {
        return;
    }
    const size_t faultsBefore = m_services.moduleHost.systemFaults().faults().size();
    if (const std::optional<std::string> failure = callGuarded(run)) {
        m_gameplayFailure = "Gameplay threw in " + std::string(stage) + ": " + *failure;
        return;
    }
    const std::span<const SystemFault> faults = m_services.moduleHost.systemFaults().faults();
    if (faults.size() > faultsBefore) {
        const SystemFault& fault = faults[faultsBefore];
        m_gameplayFailure =
            "System '" + fault.name + "' threw in " + std::string(systemPhaseName(fault.phase)) + ": " + fault.what;
    }
}

void EditorPlayback::stopFailedGameplay(ApplicationContext& context) {
    if (!m_gameplayFailure) {
        return;
    }
    const std::string failure = *std::exchange(m_gameplayFailure, std::nullopt);
    if (m_services.playMode.mode() == EditorMode::Edit) {
        return;
    }
    const char* runtime = m_services.playMode.simulating() ? "Simulate" : "Play";
    stop(context);
    m_services.diagnostics.add(EditorDiagnosticSeverity::Error, playDiagnosticSource,
                               std::string(runtime) + " stopped and the edit scene was restored. " + failure,
                               "Fix the gameplay code, rebuild the module, and start again.");
}

bool EditorPlayback::enter(ApplicationContext& context, EditorRuntime runtime) {
    context.physics.clear();
    context.audio.stopSceneVoices();
    if (const SceneDocumentResult result = m_services.playMode.enter(
            context.scene, context.assets, m_services.document.options(), m_services.selection.ids(), runtime);
        !result) {
        m_services.diagnostics.reportError(std::string(runtime == EditorRuntime::Simulate
                                                           ? "Could not enter Simulate: "
                                                           : "Could not enter Play mode: ") +
                                           result.error);
        return false;
    }
    m_services.moduleHost.startGameplay(context.assets, m_services.document.options(), context.rendering.debugDraw());
    if (GameServices* services = m_services.moduleHost.gameServices()) {
        services->outside().setAudio(&context.audio);
        // The game bakes as navigation.bake previewed, as the shipped game does.
        services->outside().setDefaultNavigationSettings(m_services.navigationSettings);
        // Play saves into the project, apart from the shipped game's saves.
        if (!m_services.projectFile.empty() && m_services.document.path()) {
            const std::filesystem::path project = m_services.projectFile.parent_path();
            services->outside().setSaveGames(SaveGameStore(project / ".zenith" / "saves", project),
                                             *m_services.document.path());
        }
        services->events().publish(SceneLoaded{});
    }
    m_services.history.crossBoundary(CommandHistoryBoundary::EnterPlayMode);
    m_services.diagnostics.acknowledgeErrors();
    m_gameplayFailure.reset();
    return true;
}

void EditorPlayback::stop(ApplicationContext& context) {
    if (m_services.playMode.mode() == EditorMode::Edit) {
        return;
    }
    context.audio.stopSceneVoices();
    context.physics.clear();
    endGameView();
    m_services.moduleHost.stopGameplay(context.scene);
    std::vector<SceneEntityId> restoredSelection;
    m_services.playMode.stop(context.scene, restoredSelection);
    m_services.selection.assign(std::move(restoredSelection));
    m_services.history.crossBoundary(CommandHistoryBoundary::LeavePlayMode);
    m_services.document.setDirty(m_services.history.dirty());
}

void EditorPlayback::update(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    EditorPlayMode& playMode = m_services.playMode;
    if (playMode.mode() == EditorMode::Edit) {
        return;
    }
    const SimulationMode mode = playMode.simulationMode();
    m_frameGameInput = mode == SimulationMode::Play ? gameInput(input) : m_idleGameInput;
    m_frameGameSeconds = deltaSeconds;
    if (!playMode.advancesThisFrame()) {
        return;
    }
    // Update systems and timers run on simulated time (a paused frame step's
    // own length, zero while the game pauses itself); the engine scales the
    // fixed steps, animation, and audio by the same clock.
    runGameplayStage(systemPhaseName(SystemPhase::Update), [&] {
        GameModuleHost& host = m_services.moduleHost;
        GameServices* services = host.gameServices();
        if (services != nullptr) {
            services->beginFrame();
        }
        host.runSystems(SystemPhase::Update, context.scene, &context, mode,
                        simulatedSeconds(simulationClock(), deltaSeconds), &m_frameGameInput, nullptr, deltaSeconds);
        if (services != nullptr) {
            // On the clock as the systems left it, so a pause they requested
            // holds the timers this frame.
            services->advanceTimers(context.scene, simulatedSeconds(simulationClock(), deltaSeconds));
        }
    });
}

void EditorPlayback::fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    // Fixed updates run only while simulating (including a paused single
    // step); Edit mode keeps its own fixed clock in the module host's edit
    // frame. Simulate passes no input; the host enforces that too.
    if (m_services.playMode.mode() == EditorMode::Edit) {
        return;
    }
    const SimulationMode mode = m_services.playMode.simulationMode();
    runGameplayStage(systemPhaseName(SystemPhase::FixedUpdate), [&] {
        m_services.moduleHost.runSystems(SystemPhase::FixedUpdate, context.scene, &context, mode, deltaSeconds,
                                         mode == SimulationMode::Simulate ? nullptr : &gameInput(input));
    });
}

void EditorPlayback::afterPhysicsStep(ApplicationContext& context) {
    if (GameServices* services = m_services.moduleHost.gameServices()) {
        runGameplayStage("the physics events", [&] {
            services->publishPhysicsEvents(context.scene, context.physics.triggerEvents(),
                                           context.physics.contactEvents());
        });
    }
}

void EditorPlayback::lateUpdate(ApplicationContext& context, float deltaSeconds) {
    EditorPlayMode& playMode = m_services.playMode;
    if (playMode.mode() == EditorMode::Edit || !playMode.advancesThisFrame()) {
        return;
    }
    const SimulationMode mode = playMode.simulationMode();
    runGameplayStage(systemPhaseName(SystemPhase::LateUpdate), [&] {
        m_services.moduleHost.runSystems(SystemPhase::LateUpdate, context.scene, &context, mode,
                                         simulatedSeconds(simulationClock(), deltaSeconds), &m_frameGameInput, nullptr,
                                         deltaSeconds);
    });
}

SimulationClock EditorPlayback::simulationClock() const {
    // The game's own pause and time scale apply on top of the editor's.
    const SimulationClock editorClock = m_services.playMode.simulationClock();
    const GameServices* services = m_services.moduleHost.gameServices();
    return services != nullptr ? services->time().simulationClock(editorClock) : editorClock;
}

void EditorPlayback::beginUiFrame(ApplicationContext& context) {
    // Between the frame's gameplay and the game view's UI: a run whose
    // gameplay failed ends here, so the rest of the frame draws Edit mode.
    stopFailedGameplay(context);
    // The engine has run this frame's fixed steps, so a frame step is done;
    // play.step during the UI requests the next one.
    m_services.playMode.finishFrame();
    // Before the areas, so the game view draws this frame's game UI.
    drawGameViewUi(context);
}

void EditorPlayback::unload(ApplicationContext& context) {
    stopPlay(context);
    if (m_services.mouseCaptureChanged) {
        m_services.mouseCaptureChanged(MouseCapturePolicy::WhileRightMouseDown);
    }
}
