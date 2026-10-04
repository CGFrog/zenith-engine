// EditorSession's game module workflows: loading the project's module,
// running its systems each frame, hot reloading it, and its menu, status, and
// panels.

#include "EditorSession.hpp"

#include "editor/application/EditorSessionOperators.hpp"
#include "editor/panels/GameModulePanel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <utility>

namespace {
constexpr const char* gameModuleDiagnosticSource = "Game Module";

std::string moduleLabel(const GameModule& module) {
    return module.version().empty() ? module.name() : module.name() + " " + module.version();
}
} // namespace

OperatorRegistrationResult registerModuleOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services) {
    const GameModuleHost& modules = services.modules;
    EditorObjectModeCommands& objectMode = services.objectModeCommands;
    EditorWindowCommands& windows = services.windowCommands;
    return registry.add(applicationOperator(
        "module.reload", "Reload Game Module",
        "Rebuild-friendly hot reload: unload the game module and load the library again, keeping the scene",
        [&modules, &objectMode](const OperatorContext& context) {
            return !modules.sourcePath().empty() && context.editMode() && !objectMode.modalTransformRunning();
        },
        // Deferred to the next update, so a module panel or operator that
        // requests it is not unloaded while it runs.
        [&windows](ApplicationContext&) { windows.requestModuleReload(); }));
}

void EditorSession::requestModuleReload() {
    m_moduleReloadPending = true;
}

std::optional<EditorSourceLocation> EditorSession::behaviourSource(std::string_view componentKey) const {
    return m_moduleHost.behaviourSource(componentKey);
}

ScriptStatus EditorSession::scriptStatus() const {
    if (m_moduleHost.status() == GameModuleStatus::Failed) {
        return ScriptStatus::ReloadFailed;
    }
    if (m_projectBuild.running()) {
        return ScriptStatus::Building;
    }
    if (m_projectBuild.runner().state() == BuildState::Failed) {
        return ScriptStatus::BuildFailed;
    }
    return ScriptStatus::Current;
}

void EditorSession::openNewCodeDialog(GeneratedCodeKind kind) {
    m_newCodeDialog.open(kind);
}

void EditorSession::drawNewCodeDialog(ApplicationContext& context) {
    const std::optional<GenerateCodeProperties> create = m_newCodeDialog.draw();
    const Operator* generate = create ? m_operators.find("module.generate_code") : nullptr;
    if (generate == nullptr) {
        return;
    }
    OperatorProperties properties = generate->properties;
    if (GenerateCodeProperties* values = properties.get<GenerateCodeProperties>()) {
        *values = *create;
    }
    m_newCodeDialog.finish(invokeOperator(context, "module.generate_code", properties));
}

void EditorSession::loadGameModule() {
    if (m_gameModulePath.empty()) {
        return;
    }
    reportGameModule(m_moduleHost.load(m_gameModulePath, m_document));
}

void EditorSession::updateGameModule(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    if (!m_moduleHost.sourcePath().empty()) {
        // A successful build reloads a module it rebuilt; the file check
        // also catches builds started outside the editor.
        const BuildState buildState = m_projectBuild.runner().state();
        if (buildState == BuildState::Succeeded && m_lastBuildState == BuildState::Running &&
            m_moduleHost.sourceChanged()) {
            m_moduleReloadPending = true;
        }
        m_lastBuildState = buildState;
        if (m_moduleHost.pollSourceChange(std::chrono::steady_clock::now())) {
            m_moduleReloadPending = true;
        }
        const bool reloadAllowed = buildState != BuildState::Running && m_playMode.mode() == EditorMode::Edit &&
                                   !m_activeTransform && !m_fileDialog.pending();
        if (m_moduleReloadPending && reloadAllowed) {
            reloadGameModule(context);
        }
    }

    if (m_playMode.mode() == EditorMode::Edit) {
        m_moduleHost.runEditFrame(context.scene, deltaSeconds, context.physics.config().fixedTimeStep);
        return;
    }
    const SimulationMode mode = m_playMode.simulationMode();
    m_frameGameInput = mode == SimulationMode::Play ? gameInput(input) : m_idleGameInput;
    if (m_playMode.advancesThisFrame()) {
        // Update systems and timers run on simulated time (a paused frame
        // step's own length); the engine scales the fixed steps, animation,
        // and audio by the same clock.
        const float seconds = simulatedSeconds(m_playMode.simulationClock(), deltaSeconds);
        GameServices* services = m_moduleHost.gameServices();
        if (services != nullptr) {
            services->beginFrame();
        }
        m_moduleHost.runSystems(SystemPhase::Update, context.scene, &context, mode, seconds, &m_frameGameInput);
        if (services != nullptr) {
            services->advanceTimers(context.scene, seconds);
        }
    }
}

void EditorSession::reloadGameModule(ApplicationContext& context) {
    m_moduleReloadPending = false;
    // Physics bodies and audio voices refer to entities of the scene the
    // reload replaces.
    context.audio.stopSceneVoices();
    context.physics.clear();
    const bool dirty = m_document.dirty();
    reportGameModule(m_moduleHost.reload(context.scene, context.assets, m_document));
    // Entity ids survive the reload; anything the restored scene lacks is
    // dropped from the selection.
    m_selection.removeMissing(context.scene);
    m_document.setDirty(dirty);
}

void EditorSession::reportGameModule(const GameModuleHostResult& result) {
    std::vector<EditorDiagnostic> entries;
    if (result) {
        entries.push_back({.severity = EditorDiagnosticSeverity::Info,
                           .message = "Loaded game module " + moduleLabel(*m_moduleHost.module()) + " from " +
                                      m_moduleHost.sourcePath().string()});
    } else {
        entries.push_back(
            {.severity = EditorDiagnosticSeverity::Error,
             .message = result.error,
             .action = "Fix or rebuild the module; it reloads when the library changes or with Module > Reload. "
                       "Its components are kept unedited until then."});
    }
    for (const std::string& warning : result.warnings) {
        entries.push_back({.severity = EditorDiagnosticSeverity::Warning, .message = warning});
    }
    m_diagnostics.replaceSource(gameModuleDiagnosticSource, std::move(entries));
}

std::string EditorSession::gameModuleStatusText() const {
    std::string text;
    switch (m_moduleHost.status()) {
    case GameModuleStatus::None:
        return "No game module";
    case GameModuleStatus::Loaded:
        text = "Module: " + moduleLabel(*m_moduleHost.module());
        break;
    case GameModuleStatus::Failed:
        text = "Module: " + m_moduleHost.sourcePath().filename().string() + " failed to load";
        break;
    }
    if (m_moduleReloadPending) {
        text += " (reload pending)";
    }
    return text;
}

void EditorSession::drawModuleMenu(ApplicationContext& context) {
    if (!ImGui::BeginMenu("Module")) {
        return;
    }
    ImGui::TextDisabled("%s", gameModuleStatusText().c_str());
    if (m_moduleHost.status() == GameModuleStatus::Failed) {
        ImGui::SetItemTooltip("%s", m_moduleHost.error().c_str());
    }
    operatorMenuItem(context, "build.run", "Build");
    operatorMenuItem(context, "module.reload", "Reload Game Module");
    ImGui::Separator();
    operatorMenuItem(context, "module.new_behaviour", "New Behaviour...");
    operatorMenuItem(context, "module.new_component", "New Component...");
    operatorMenuItem(context, "module.new_system", "New System...");
    // Module panels are area types (see syncAreaTypes), toggled like any
    // other in the active workspace.
    std::span<GameModuleHost::Panel> panels = m_moduleHost.panels();
    if (!panels.empty()) {
        ImGui::Separator();
        for (const GameModuleHost::Panel& panel : panels) {
            const std::string typeId = gameModuleAreaTypeId(panel.panel.title);
            operatorMenuItem(context, "wm.toggle_panel", panel.panel.title.c_str(),
                             nlohmann::json{{"panel", typeId}}.dump(), m_workspaces.active().anyVisible(typeId));
        }
    }
    // The module's own menus open as popups, like any wm.call_menu binding.
    std::span<const EditorModuleMenu> menus = m_moduleHost.menus();
    if (!menus.empty()) {
        ImGui::Separator();
        for (const EditorModuleMenu& menu : menus) {
            const std::string label = (menu.title.empty() ? menu.id : menu.title) + "...";
            operatorMenuItem(context, "wm.call_menu", label.c_str(), nlohmann::json{{"menu", menu.id}}.dump());
        }
    }
    ImGui::EndMenu();
}
