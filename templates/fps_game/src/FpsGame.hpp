#pragma once

#include "engine/scene/SceneEntityId.hpp"

#include <string>
#include <string_view>

class ModuleRegistrar;
class Scene;

// The events the game's parts tell each other about. Events published in one
// frame are read in the next (EventQueue).
struct TargetHit {
    static constexpr std::string_view eventKey = "{{PROJECT_IDENTIFIER}}.target_hit";
    SceneEntityId target;
    float healthLeft{0.0f};
};

struct TargetDestroyed {
    static constexpr std::string_view eventKey = "{{PROJECT_IDENTIFIER}}.target_destroyed";
    SceneEntityId target;
};

struct GoalReached {
    static constexpr std::string_view eventKey = "{{PROJECT_IDENTIFIER}}.goal_reached";
};

enum class FpsScreen {
    Playing,
    Paused,
    Settings,
};

// The round's state: one per scene, kept in the registry's context, so a new
// Play session (a fresh copy of the scene) starts a new round.
struct FpsGameState {
    FpsScreen screen{FpsScreen::Playing};
    int targetsTotal{0};
    int targetsDestroyed{0};
    bool goalReached{false};
    // A line the HUD shows for a moment.
    std::string message;
    float messageSeconds{0.0f};
};

FpsGameState& fpsGameState(Scene& scene);

// Registers the round's systems: "score" (Update) counts destroyed targets
// and the goal and toggles the pause screen; "hud" (Ui) draws the crosshair,
// the score, messages, and the pause and settings menus.
void registerFpsGame(ModuleRegistrar& registrar);
