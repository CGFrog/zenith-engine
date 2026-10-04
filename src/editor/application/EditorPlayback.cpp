// EditorSession's play workflows: the play operators and their targets (the
// active viewport, a floating viewport, or a standalone game process), game
// views and the input they route to module systems, the mouse capture
// policy, the playback toolbar in the main menu bar, and the play target
// smoke.

#include "EditorSession.hpp"

#include "editor/panels/SimulateSystemPicker.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <limits>

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

void EditorSession::registerPlayOperatorsOfSession() {
    std::string targetKeys;
    for (const PlayTargetInfo& target : playTargets()) {
        targetKeys += targetKeys.empty() ? "" : ", ";
        targetKeys += "\"" + std::string(target.key) + "\" (" + std::string(target.label) + ")";
    }
    registerOperator(
        {.id = "play.start",
         .label = "Play",
         .description = "Run the scene on a copy of the document in a play target: " + targetKeys +
                        "; without a target, the one used last",
         .properties = OperatorProperties::make(PlayStartProperties{}, std::move(Fields<PlayStartProperties>().add(
                                                                           "target", &PlayStartProperties::target))),
         .poll =
             [this](const OperatorContext& context) {
                 return context.app != nullptr && context.mode == EditorMode::Edit && !m_standalone.running();
             },
         .execute =
             [this, targetKeys](OperatorContext& context, OperatorProperties& properties) {
                 const PlayStartProperties* values = properties.get<PlayStartProperties>();
                 if (values == nullptr) {
                     return OperatorResult::failed("Operator properties have the wrong type");
                 }
                 PlayTarget target = m_lastPlayTarget;
                 if (!values->target.empty()) {
                     const std::optional<PlayTarget> parsed = parsePlayTarget(values->target);
                     if (!parsed) {
                         return OperatorResult::failed("Unknown play target '" + values->target + "'; use " +
                                                       targetKeys);
                     }
                     target = *parsed;
                 }
                 return startPlay(*context.app, target);
             }});
    registerOperator(applicationOperator(
        "play.simulate", "Simulate",
        "Run physics and the systems chosen in the System Profiler on a copy of the document, keeping the editor "
        "camera and tools",
        [this](const OperatorContext& context) { return context.mode == EditorMode::Edit && !m_standalone.running(); },
        [this](ApplicationContext& app) { enterPlayMode(app, EditorRuntime::Simulate); }));
    registerOperator(applicationOperator(
        "play.toggle_pause", "Pause / Resume", "Pause or resume Play or Simulate",
        [](const OperatorContext& context) { return context.mode != EditorMode::Edit; },
        [this](ApplicationContext&) {
            if (m_playMode.running()) {
                m_playMode.pause();
            } else {
                m_playMode.resume();
            }
        }));
    registerOperator(applicationOperator(
        "play.step", "Step Frame",
        "Advance paused Play or Simulate by one frame of the fixed time step: Update systems, animation, and "
        "audio, plus the fixed update that frame runs",
        [](const OperatorContext& context) { return context.mode == EditorMode::Paused; },
        [this](ApplicationContext& app) { m_playMode.requestFrameStep(app.physics.config().fixedTimeStep); }));
    registerOperator(applicationOperator(
        "play.stop", "Stop", "Leave Play or Simulate and restore the authored scene, or stop the standalone game",
        [this](const OperatorContext&) { return playActive(); }, [this](ApplicationContext& app) { stopPlay(app); }));
    registerOperator({.id = "play.focus_game_view",
                      .label = "Focus Game View",
                      .description = "Give the game view player input; a click in it then captures the cursor",
                      .flags = OperatorFlags::Internal,
                      .poll = [this](const OperatorContext&) { return gameViewport() != nullptr; },
                      .execute =
                          [this](OperatorContext&, OperatorProperties&) {
                              gameViewport()->setGameInputFocus(true);
                              return OperatorResult::finished();
                          }});
    registerOperator({.id = "play.release_game_input",
                      .label = "Release Game Input",
                      .description = "Take player input and the cursor back from the game view",
                      .flags = OperatorFlags::Internal,
                      .poll =
                          [this](const OperatorContext&) {
                              const ViewportPanel* viewport = gameViewport();
                              return viewport != nullptr && viewport->gameInputFocus();
                          },
                      .execute =
                          [this](OperatorContext&, OperatorProperties&) {
                              gameViewport()->setGameInputFocus(false);
                              return OperatorResult::finished();
                          }});
}

bool EditorSession::playActive() const {
    return m_playMode.mode() != EditorMode::Edit || m_standalone.running();
}

OperatorResult EditorSession::startPlay(ApplicationContext& context, PlayTarget target) {
    m_lastPlayTarget = target;
    if (target == PlayTarget::Standalone) {
        return startStandalone(context);
    }
    ViewportPanel* viewport = m_viewports.active();
    if (target == PlayTarget::FloatingViewport) {
        Workspace& workspace = m_workspaces.active();
        const AreaResult opened = workspace.openNew(m_areaTypes, "viewport");
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
    if (!enterPlayMode(context)) {
        endGameView();
        return OperatorResult::failed("Play mode could not start");
    }
    m_activePlayTarget = target;
    if (viewport == nullptr) {
        m_diagnostics.add(EditorDiagnosticSeverity::Info, playDiagnosticSource,
                          "No viewport is open, so the game runs without a view.");
        return OperatorResult::finished();
    }
    viewport->beginGameView();
    m_gameViewport = viewport->id();
    m_viewports.activate(m_gameViewport);
    return OperatorResult::finished();
}

OperatorResult EditorSession::startStandalone(ApplicationContext& context) {
    const StandaloneResult started = m_standalone.start({.gameExecutable = m_playServices.gameExecutable,
                                                         .projectFile = m_projectFile,
                                                         .temporaryDirectory = playTemporaryDirectory(m_playServices)},
                                                        context.scene, context.assets, m_document.options());
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

void EditorSession::stopPlay(ApplicationContext& context) {
    if (m_standalone.running()) {
        // The session leaves Play once updateStandalone sees the process end.
        m_standalone.stop();
    }
    stopPlayMode(context);
}

void EditorSession::endGameView() {
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
    if (Workspace* workspace = m_workspaces.find(m_floatingPlayWorkspace)) {
        // Hides rather than removes it when the user closed every other
        // viewport meanwhile; either way it leaves the screen.
        workspace->close(m_floatingPlayArea);
        m_workspacesDirty = true;
    }
    m_floatingPlayArea.clear();
    m_floatingPlayWorkspace.clear();
}

ViewportPanel* EditorSession::gameViewport() const {
    ViewportPanel* viewport = m_gameViewport ? m_viewports.find(m_gameViewport) : nullptr;
    return viewport != nullptr && viewport->gameView() ? viewport : nullptr;
}

const InputState& EditorSession::gameInput(const InputState& input) const {
    if (!m_gameViewport) {
        // Play without a view (no viewport was open) keeps the old routing.
        return input;
    }
    const ViewportPanel* viewport = gameViewport();
    return viewport != nullptr && viewport->gameInputFocus() ? input : m_idleGameInput;
}

MouseCapturePolicy EditorSession::mouseCapturePolicy() const {
    const ViewportPanel* viewport = gameViewport();
    return playMouseCapturePolicy(viewport != nullptr && viewport->gameViewHoldsCursor());
}

void EditorSession::publishMouseCapture() {
    // Every frame rather than on change: a session whose replacement failed
    // to load (and reset the policy while unloading) restores its own.
    if (m_mouseCaptureChanged) {
        m_mouseCaptureChanged(mouseCapturePolicy());
    }
}

void EditorSession::updateStandalone(ApplicationContext& context) {
    if (!m_standalone.poll(context.logger)) {
        return;
    }
    if (m_activePlayTarget == PlayTarget::Standalone) {
        m_activePlayTarget.reset();
    }
}

void EditorSession::drawPlaybackToolbar(ApplicationContext& context) {
    ImGui::Separator();
    const float side = ImGui::GetFrameHeight();
    const ImVec2 square{side, side};
    const EditorMode mode = m_playMode.mode();
    const auto button = [&](const char* id, EditorIcon icon, bool active, std::string_view operatorId,
                            const char* tooltip, std::string_view properties = {}) {
        ImGui::BeginDisabled(!operatorAvailable(context, operatorId));
        if (widgets::ToolButton(id, icon, active, square)) {
            invokeOperator(context, operatorId, properties, KeymapRegion::Global);
        }
        ImGui::EndDisabled();
        const std::string shortcut = m_keymap.shortcutLabel(operatorId, properties);
        ImGui::SetItemTooltip("%s%s%s", tooltip, shortcut.empty() ? "" : "  ", shortcut.c_str());
    };
    const auto dropdown = [&](const char* id, const char* popup) {
        ImGui::SameLine(0.0f, 0.0f);
        if (widgets::Button(id, {side * 0.6f, side})) {
            ImGui::OpenPopup(popup);
        }
    };

    const bool playing = mode == EditorMode::Play || (mode == EditorMode::Paused && !m_playMode.simulating());
    const bool simulating = mode == EditorMode::Simulate || (mode == EditorMode::Paused && m_playMode.simulating());
    const std::string playTooltip = "Play in " + std::string(playTargetInfo(m_lastPlayTarget).label);
    button("##toolbar_play", EditorIcon::Play, playing || m_standalone.running(), "play.start", playTooltip.c_str());
    dropdown("v##toolbar_play_targets", "##PlayTargetMenu");
    if (ImGui::BeginPopup("##PlayTargetMenu")) {
        for (const PlayTargetInfo& target : playTargets()) {
            operatorMenuItem(context, "play.start", std::string(target.label).c_str(),
                             nlohmann::json{{"target", target.key}}.dump(), target.target == m_lastPlayTarget);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    button("##toolbar_simulate", EditorIcon::Physics, simulating, "play.simulate", "Simulate");
    dropdown("v##toolbar_simulate_options", "##SimulateOptions");
    if (ImGui::BeginPopup("##SimulateOptions")) {
        drawSimulateOptions(context);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    button("##toolbar_pause", EditorIcon::Pause, mode == EditorMode::Paused, "play.toggle_pause",
           mode == EditorMode::Paused ? "Resume" : "Pause");
    ImGui::SameLine();
    ImGui::BeginDisabled(!operatorAvailable(context, "play.step"));
    if (widgets::Button("Step##toolbar_step", {0.0f, side})) {
        invokeOperator(context, "play.step", {}, KeymapRegion::Global);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Step one frame while paused  %s", m_keymap.shortcutLabel("play.step", {}).c_str());
    ImGui::SameLine();
    button("##toolbar_stop", EditorIcon::Stop, false, "play.stop", "Stop");
    ImGui::SameLine();
    float scale = m_playMode.timeScale();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    if (ImGui::DragFloat("##toolbar_time_scale", &scale, 0.01f, SimulationClock::minimumTimeScale,
                         SimulationClock::maximumTimeScale, "x%.2f")) {
        invokeOperator(context, "play.set_time_scale", nlohmann::json{{"scale", scale}}.dump(), KeymapRegion::Global);
    }
    ImGui::SetItemTooltip("Time scale for Play and Simulate");
}

void EditorSession::drawSimulateOptions(ApplicationContext& context) {
    operatorMenuItem(context, "play.simulate", "Simulate");
    ImGui::Separator();
    EditorContext editor = editorContext(context);
    drawSimulateSystemPicker(editor, m_moduleHost);
}

bool EditorSession::updateSmokePlayTargets(ApplicationContext& context) {
    // Frames 350-420: Play in the active viewport (it switches to the game
    // camera, holds game input with Persistent mouse capture, shows its
    // banner, pauses and steps one frame, and Stop restores the scene and
    // the editor camera); then, when the game executable is built, play
    // standalone: launch it on a temporary scene, stream its output for a
    // while, and stop it.
    constexpr uint32_t firstFrame = 350;
    constexpr uint32_t standaloneFrame = firstFrame + 6;
    constexpr uint32_t standaloneStopFrame = firstFrame + 50;
    constexpr uint32_t lastFrame = firstFrame + 70;
    const auto fail = [&](const std::string& message) {
        const std::string error = "Play target smoke: " + message;
        std::cerr << error << '\n';
        context.logger.log(LogLevel::Error, "editor", error);
        m_diagnostics.reportError(error);
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    };
    const auto snapshot = [&] {
        std::string text;
        if (const SceneDocumentResult result =
                SceneDocument::serialize(context.scene, context.assets, m_document.options(), text);
            !result) {
            fail("the scene could not be serialized: " + result.error);
        }
        return text;
    };
    if (m_smokePlayTargetsDone || m_smokeFrame < firstFrame) {
        return m_smokePlayTargetsDone;
    }
    const auto finish = [&] {
        m_smokePlayTargetsDone = true;
        return true;
    };
    ViewportPanel* viewport = gameViewport();
    if (m_smokeFrame == firstFrame) {
        if (m_viewports.active() == nullptr) {
            context.logger.log(LogLevel::Info, "editor", "Play target smoke: viewport phase skipped, no viewport");
            return false;
        }
        m_smokePlaySnapshot = snapshot();
        if (!invokeOperator(context, "play.start", R"({"target":"viewport"})", KeymapRegion::Global) ||
            m_playMode.mode() != EditorMode::Play || gameViewport() == nullptr) {
            fail("play.start did not play in the active viewport");
        } else if (const InputState frameInput;
                   !gameViewport()->gameInputFocus() || &gameInput(frameInput) != &frameInput) {
            fail("the game view should start with input focus, routing input to the game");
        }
    } else if (m_smokeFrame == firstFrame + 2 && viewport != nullptr) {
        // The view has been drawn in game view since.
        const Entity camera = gameCamera(context.scene);
        const std::optional<RenderView> cameraLens = sceneCameraLens(context.scene, camera);
        if (cameraLens && glm::length(viewport->lens(context.scene).position - cameraLens->position) > 1e-3f) {
            fail("the game view does not look through the game camera");
        }
        for (const ViewportPanel* other : m_viewports.openViewports()) {
            if (other != viewport && other->gameView()) {
                fail("only the playing viewport should show the game");
            }
        }
        const GameViewBanner banner = viewport->gameViewBanner();
        if (banner == GameViewBanner::None) {
            fail("the game view shows no input banner");
        }
        context.logger.log(LogLevel::Info, "editor",
                           std::string("Play target smoke: game camera ") + (cameraLens ? "found" : "absent") +
                               ", banner '" + std::string(gameViewBannerText(banner)) + "'");
        invokeOperator(context, "play.release_game_input", {}, KeymapRegion::Global);
        const InputState frameInput;
        if (mouseCapturePolicy() != MouseCapturePolicy::WhileRightMouseDown ||
            &gameInput(frameInput) != &m_idleGameInput) {
            fail("releasing game input should restore editor mouse capture and stop routing input");
        }
        invokeOperator(context, "play.toggle_pause", {}, KeymapRegion::Global);
    } else if (m_smokeFrame == firstFrame + 3 && viewport != nullptr) {
        if (!invokeOperator(context, "play.step", {}, KeymapRegion::Global) || !m_playMode.steppingFrame()) {
            fail("play.step did not request a frame step while paused");
        }
    } else if (m_smokeFrame == firstFrame + 4 && viewport != nullptr) {
        if (m_playMode.mode() != EditorMode::Paused || m_playMode.steppingFrame()) {
            fail("a frame step should last one frame and stay paused");
        }
        invokeOperator(context, "play.stop", {}, KeymapRegion::Global);
        if (m_playMode.mode() != EditorMode::Edit || viewport->gameView() || snapshot() != m_smokePlaySnapshot) {
            fail("Stop did not end the game view and restore the scene");
        }
    } else if (m_smokeFrame == standaloneFrame) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(m_playServices.gameExecutable, error)) {
            context.logger.log(LogLevel::Info, "editor",
                               "Play target smoke: standalone phase skipped, the game executable '" +
                                   m_playServices.gameExecutable.string() + "' is not built");
            return finish();
        }
        m_smokeGameLogSequence = context.logger.snapshotAfter(std::numeric_limits<uint64_t>::max()).latestSequence;
        m_smokeStandaloneFrame = m_smokeFrame;
        if (!invokeOperator(context, "play.start", R"({"target":"standalone"})", KeymapRegion::Global) ||
            !m_standalone.running() || !std::filesystem::is_regular_file(m_standalone.sceneFile(), error)) {
            fail("play.start did not launch the standalone game on a temporary scene");
            return finish();
        }
    } else if (m_smokeFrame > standaloneFrame && m_smokeStandaloneFrame != 0) {
        if (!m_standalone.running()) {
            // Ended by itself or after Stop: the session left Play.
            size_t loggedLines = 0;
            for (const LogEntry& entry : context.logger.entriesAfter(m_smokeGameLogSequence)) {
                loggedLines += entry.category == StandalonePlayer::logCategory ? 1 : 0;
            }
            context.logger.log(LogLevel::Info, "editor",
                               "Play target smoke: the standalone game ran for " +
                                   std::to_string(m_smokeFrame - m_smokeStandaloneFrame) + " editor frames; " +
                                   std::to_string(m_standalone.outputLines()) + " output line(s) reached the log");
            if (playActive() || m_activePlayTarget || loggedLines < m_standalone.outputLines()) {
                fail("the standalone game's end should leave Play with its output in the log");
            }
            return finish();
        }
        // Stop once the game has run a while and printed something, or at
        // the end of the window.
        const bool ranAWhile = m_standalone.outputLines() > 0 && m_smokeFrame >= m_smokeStandaloneFrame + 30;
        if (!m_standalone.stopRequested() && (ranAWhile || m_smokeFrame >= standaloneStopFrame)) {
            invokeOperator(context, "play.stop", {}, KeymapRegion::Global);
        } else if (m_smokeFrame >= lastFrame) {
            fail("the standalone game did not end after Stop");
            return finish();
        }
    }
    return false;
}
