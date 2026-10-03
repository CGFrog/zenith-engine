// EditorSession's game module workflows: loading the project's module,
// running its systems each frame, hot reloading it, and its menu, status, and
// panels.

#include "EditorSession.hpp"

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

void EditorSession::registerModuleOperators() {
    registerOperator(applicationOperator(
        "module.reload", "Reload Game Module",
        "Rebuild-friendly hot reload: unload the game module and load the library again, keeping the scene",
        [this](const OperatorContext& context) {
            return !m_moduleHost.sourcePath().empty() && context.editMode() && !m_activeTransform;
        },
        // Deferred to the next update, so a module panel or operator that
        // requests it is not unloaded while it runs.
        [this](ApplicationContext&) { m_moduleReloadPending = true; }));
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
        const BuildState buildState = m_buildPanel.runner().state();
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
    } else if (m_playMode.mode() == EditorMode::Play) {
        m_moduleHost.runSystems(SystemPhase::Update, context.scene, &context, SimulationMode::Play, deltaSeconds,
                                &input);
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
    operatorMenuItem(context, "module.reload", "Reload Game Module");
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
    ImGui::EndMenu();
}
