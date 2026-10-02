// EditorSession's game module workflows: loading the project's module,
// running its systems each frame, hot reloading it, and its menu, status, and
// panels.

#include "EditorSession.hpp"

#include <imgui.h>

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

void EditorSession::updateGameModule(ApplicationContext& context, float deltaSeconds) {
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
        m_moduleHost.runSystems(SystemPhase::Update, context.scene, &context, SimulationMode::Play, deltaSeconds);
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
    std::span<GameModuleHost::Panel> panels = m_moduleHost.panels();
    if (!panels.empty()) {
        ImGui::Separator();
        for (GameModuleHost::Panel& panel : panels) {
            ImGui::MenuItem(panel.panel.title.c_str(), nullptr, &panel.visible);
        }
    }
    ImGui::EndMenu();
}

void EditorSession::drawModulePanels(ApplicationContext& context) {
    // Module panels draw module code; a reload requested while they draw is
    // deferred to the next update, so the span stays valid here.
    for (GameModuleHost::Panel& panel : m_moduleHost.panels()) {
        if (!panel.visible) {
            continue;
        }
        // The suffix keeps module windows distinct from editor panels of the
        // same title.
        const std::string window = panel.panel.title + "##game_module";
        if (ImGui::Begin(window.c_str(), &panel.visible)) {
            // Copied: an operator the panel invokes may change the selection.
            const std::vector<SceneEntityId> selection = m_selection.ids();
            EditorModulePanelContext panelContext{
                .scene = context.scene,
                .assets = context.assets,
                .selection = selection,
                .editMode = m_playMode.mode() == EditorMode::Edit,
                .invokeOperator = [this, &context](std::string_view operatorId) {
                    return static_cast<bool>(invokeOperator(context, operatorId, {}, KeymapRegion::Global));
                }};
            panel.panel.draw(panelContext);
        }
        ImGui::End();
    }
}
