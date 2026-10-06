#pragma once

// Chooses the gameplay a run plays and the application that presents it (the demo's or ProjectApplication).

#include "engine/module/GameModule.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/project/ProjectConfig.hpp"

#include <memory>
#include <string_view>

// The application that presents the gameplay: the demo's (title, pause, and
// settings menus, the item HUD, a first-person player, music), or the
// project-agnostic ProjectApplication, which only plays the scene and runs the
// gameplay's systems.
enum class GamePresentation {
    Demo,
    Project,
};

// The game module name (ZENITH_GAME_MODULE) the demo presentation is chosen
// for: the zenith_game_demo module's.
inline constexpr std::string_view demoGameModuleName = "demo";

// The gameplay the zenith executable plays, and the game module whose code it
// holds. Its owner must keep it alive until every scene the gameplay touched
// is destroyed, including the engine's own scene: declare it before the
// Engine and the application.
struct RuntimeGameplay {
    // Null when the gameplay is linked into the executable.
    std::unique_ptr<GameModule> module;
    // Declared after module, so it is released first.
    Gameplay gameplay;
    GamePresentation presentation{GamePresentation::Demo};
};

// Chooses and loads what the executable plays for project (null for the
// built-in demo scene):
// - without a project, or for a project without a game module (such as the
//   renderer showcase), the demo gameplay linked into the executable,
//   presented by the demo, so the scene can be walked with its first-person
//   player;
// - for a project with a game module, that module alone, loaded with loading;
//   presented by the demo when it is the demo's module, and by the
//   project-agnostic ProjectApplication otherwise.
// Leaves runtime unchanged on failure.
GameplayResult loadRuntimeGameplay(const ProjectConfig* project, GameModuleLoading loading, RuntimeGameplay& runtime);

// How the executable loads a project's module: in place when resourceRoot is a
// release package (it holds content-manifest.json) that ships the module (a
// packaged game, whose module never changes), and from a shadow copy
// otherwise, so a development module, even one built inside the build tree,
// can be rebuilt while the game runs.
GameModuleLoading runtimeModuleLoading(const std::filesystem::path& module, const std::filesystem::path& resourceRoot);
