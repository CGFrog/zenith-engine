// The project's game module in the editor: loading it, building it when it
// fails to load, hot reloading it after a build or a change on disk once the
// session allows, and reporting each load under "Game Module".

#include "GameModuleController.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/settings/SimulateSystemSettings.hpp"
#include "engine/application/Application.hpp"

#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace {
constexpr const char* gameModuleDiagnosticSource = "Game Module";

std::string moduleLabel(const GameModule& module) {
    return module.version().empty() ? module.name() : module.name() + " " + module.version();
}
} // namespace

GameModuleController::GameModuleController(GameModuleServices services)
    : m_services(std::move(services))
    , m_host(m_services.editor) {}

std::optional<EditorSourceLocation> GameModuleController::behaviourSource(std::string_view componentKey) const {
    return m_host.behaviourSource(componentKey);
}

ScriptStatus GameModuleController::scriptStatus() const {
    if (m_host.status() == GameModuleStatus::Failed) {
        return ScriptStatus::ReloadFailed;
    }
    if (m_services.build.running()) {
        return ScriptStatus::Building;
    }
    if (m_services.build.runner().state() == BuildState::Failed) {
        return ScriptStatus::BuildFailed;
    }
    return ScriptStatus::Current;
}

GameModuleHost& GameModuleController::host() {
    return m_host;
}

const GameModuleHost& GameModuleController::host() const {
    return m_host;
}

void GameModuleController::loadSimulateSystems() {
    SimulateSystemMask simulateMask;
    if (const SimulateSystemSettingsResult result =
            loadSimulateSystemSettings(m_services.simulateSystemsPath, simulateMask);
        !result) {
        m_services.diagnostics.reportError(result.error);
    }
    m_host.setSimulateMask(std::move(simulateMask));
}

void GameModuleController::saveSimulateSystems() {
    if (const SimulateSystemSettingsResult result =
            saveSimulateSystemSettings(m_services.simulateSystemsPath, m_host.simulateMask());
        !result) {
        m_services.diagnostics.reportError(result.error);
    }
}

void GameModuleController::load() {
    if (m_services.modulePath.empty()) {
        return;
    }
    const GameModuleHostResult result = m_host.load(m_services.modulePath, m_services.document);
    report(result);
    // A new project's module is not built yet, and one built against another
    // engine version is refused. Build it now with the project's own build,
    // so Play works without a manual first build; a successful build reloads
    // the module. A user-configured build command is left to the user.
    if (!result && m_services.build.usesProjectBuild()) {
        m_services.build.request();
        m_services.diagnostics.add(
            EditorDiagnosticSeverity::Info, gameModuleDiagnosticSource,
            "Building the game module; it loads, and Play runs the game, when the build finishes.");
    }
}

void GameModuleController::requestReload() {
    m_reloadPending = true;
}

bool GameModuleController::reloadPending() const {
    return m_reloadPending;
}

void GameModuleController::pollReload(ApplicationContext& context) {
    if (m_host.sourcePath().empty()) {
        return;
    }
    // A successful build reloads a module it rebuilt; the file check also
    // catches builds started outside the editor.
    const BuildState buildState = m_services.build.runner().state();
    if (buildState == BuildState::Succeeded && m_lastBuildState == BuildState::Running && m_host.sourceChanged()) {
        m_reloadPending = true;
    }
    m_lastBuildState = buildState;
    if (m_host.pollSourceChange(std::chrono::steady_clock::now())) {
        m_reloadPending = true;
    }
    if (m_reloadPending && buildState != BuildState::Running && m_services.reloadAllowed()) {
        reload(context);
    }
}

void GameModuleController::reload(ApplicationContext& context) {
    m_reloadPending = false;
    // Physics bodies and audio voices refer to entities of the scene the
    // reload replaces.
    context.audio.stopSceneVoices();
    context.physics.clear();
    const bool dirty = m_services.document.dirty();
    report(m_host.reload(context.scene, context.assets, m_services.document));
    // Entity ids survive the reload; anything the restored scene lacks is
    // dropped from the selection.
    m_services.selection.removeMissing(context.scene);
    m_services.document.setDirty(dirty);
}

void GameModuleController::report(const GameModuleHostResult& result) {
    std::vector<EditorDiagnostic> entries;
    if (result && result.reloadTimings) {
        const GameModuleReloadTimings& timings = *result.reloadTimings;
        char text[160];
        std::snprintf(text, sizeof(text), " in %.0f ms (snapshot %.0f, release %.0f, load %.0f, restore %.0f)",
                      timings.total(), timings.snapshot, timings.release, timings.load, timings.restore);
        entries.push_back({.severity = EditorDiagnosticSeverity::Info,
                           .message = "Reloaded game module " + moduleLabel(*m_host.module()) + text});
    } else if (result) {
        entries.push_back({.severity = EditorDiagnosticSeverity::Info,
                           .message = "Loaded game module " + moduleLabel(*m_host.module()) + " from " +
                                      m_host.sourcePath().string()});
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
    m_services.diagnostics.replaceSource(gameModuleDiagnosticSource, std::move(entries));
}

std::string GameModuleController::statusText() const {
    std::string text;
    switch (m_host.status()) {
    case GameModuleStatus::None:
        return "No game module";
    case GameModuleStatus::Loaded:
        text = "Module: " + moduleLabel(*m_host.module());
        break;
    case GameModuleStatus::Failed:
        text = "Module: " + m_host.sourcePath().filename().string() + " failed to load";
        break;
    }
    if (m_reloadPending) {
        text += " (reload pending)";
    }
    return text;
}

void GameModuleController::invalidatePrefab(const std::filesystem::path& file) {
    // Editor prefab operations build a fresh PrefabSystem each time; the
    // game's lives as long as the module host.
    if (GameServices* services = m_host.gameServices()) {
        services->prefabs().invalidate(file);
    }
}
