#pragma once

// Whether the game module's code matches its sources, for the inspector's behaviour headers.

#include "editor/application/EditorDiagnostics.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// Whether the game module's code matches its sources, for the inspector's
// behaviour headers.
enum class ScriptStatus : uint8_t {
    // The module is loaded and no build is running or failed since.
    Current,
    // A build is running; a reload follows when it succeeds.
    Building,
    // The last build failed, so the loaded behaviours are older than their
    // sources.
    BuildFailed,
    // The module failed to load; its behaviours are kept as preserved JSON
    // until a reload succeeds.
    ReloadFailed,
};

// What the inspector asks about the game module's behaviours, so the view
// needs neither the module host nor the build. The session implements it
// from GameModuleHost and ProjectBuild and answers from their current state,
// never from a cached registry.
class EditorScriptInfo {
  public:
    // Where the behaviour with this component key was registered, for Edit
    // Script; nullopt for other component types or without a loaded module.
    virtual std::optional<EditorSourceLocation> behaviourSource(std::string_view componentKey) const = 0;
    virtual ScriptStatus scriptStatus() const = 0;

  protected:
    ~EditorScriptInfo() = default;
};

// The behaviour header badge for status: nullptr when Current.
inline const char* scriptStatusBadge(ScriptStatus status) {
    switch (status) {
    case ScriptStatus::Current:
        return nullptr;
    case ScriptStatus::Building:
        return "Building";
    case ScriptStatus::BuildFailed:
        return "Build failed";
    case ScriptStatus::ReloadFailed:
        return "Reload failed";
    }
    return nullptr;
}
