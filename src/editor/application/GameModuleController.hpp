#pragma once

// The project's game module in the editor: loading it, hot reloading it after a build, and its status.

#include "editor/modules/EditorScriptInfo.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/project/BuildRunner.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class EditorDiagnostics;
class EditorDocument;
class ProjectBuild;
class Selection;
struct ApplicationContext;

// What a GameModuleController is built with. References outlive it.
struct GameModuleServices {
    // The registries the module host adds the module's editor extensions to.
    GameModuleEditorServices editor;
    ProjectBuild& build;
    EditorDiagnostics& diagnostics;
    EditorDocument& document;
    Selection& selection;
    // The project's module library; empty for none.
    std::filesystem::path modulePath;
    // Where the user's Simulate system choices are kept.
    std::filesystem::path simulateSystemsPath;
    // Whether the session can take a reload now (Edit mode, no modal tool or
    // file dialog, no prefab windows, whose scenes hold module storage the
    // reload would not restore); a requested reload waits until it can.
    std::function<bool()> reloadAllowed;
};

// Owns the GameModuleHost and decides when it reloads: after a build that
// rebuilt the module, when the library changes on disk, or on request, once
// the session allows it and no build runs. Reports loads and reloads under
// the "Game Module" diagnostics source. The session draws the Module menu
// and the status bar from the accessors.
class GameModuleController final : public EditorScriptInfo {
  public:
    explicit GameModuleController(GameModuleServices services);
    GameModuleController(const GameModuleController&) = delete;
    GameModuleController& operator=(const GameModuleController&) = delete;

    // EditorScriptInfo
    std::optional<EditorSourceLocation> behaviourSource(std::string_view componentKey) const override;
    ScriptStatus scriptStatus() const override;

    GameModuleHost& host();
    const GameModuleHost& host() const;

    // Session load: the saved Simulate choices, then the module itself. A
    // module that fails to load is built with the project's own build, which
    // reloads it when it finishes.
    void loadSimulateSystems();
    void saveSimulateSystems();
    void load();

    // Reloads at the next update the session allows.
    void requestReload();
    bool reloadPending() const;
    // Once per frame, before the module's systems run: notes a finished
    // build or changed library and reloads when the session allows it.
    void pollReload(ApplicationContext& context);
    // "Module: <name> <version>", with "(reload pending)" while one waits.
    std::string statusText() const;
    // The game's cached copy of the prefab file is stale.
    void invalidatePrefab(const std::filesystem::path& file);

  private:
    void reload(ApplicationContext& context);
    void report(const GameModuleHostResult& result);

    GameModuleServices m_services;
    GameModuleHost m_host;
    bool m_reloadPending{false};
    BuildState m_lastBuildState{BuildState::Idle};
};
