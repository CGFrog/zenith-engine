// EditorSession's play workflows: the play operators and their targets (the
// active viewport, a floating viewport, or a standalone game process), game
// views and the input they route to module systems, the mouse capture
// policy, and the playback toolbar in the main menu bar.

#include "EditorSession.hpp"

#include "editor/application/EditorSessionOperators.hpp"
#include "editor/panels/SimulateSystemPicker.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

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
             [&play](const OperatorContext& context) {
                 return context.app != nullptr && context.mode == EditorMode::Edit && !play.standaloneRunning();
             },
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
        [&play](const OperatorContext& context) {
            return context.mode == EditorMode::Edit && !play.standaloneRunning();
        },
        [&play](ApplicationContext& app) { play.startSimulate(app); }));
    operators.push_back(applicationOperator(
        "play.toggle_pause", "Pause / Resume", "Pause or resume Play or Simulate",
        [](const OperatorContext& context) { return context.mode != EditorMode::Edit; },
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
        [](const OperatorContext& context) { return context.mode == EditorMode::Paused; },
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

bool EditorSession::standaloneRunning() const {
    return m_standalone.running();
}

PlayTarget EditorSession::lastPlayTarget() const {
    return m_lastPlayTarget;
}

void EditorSession::startSimulate(ApplicationContext& context) {
    enterPlayMode(context, EditorRuntime::Simulate);
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
