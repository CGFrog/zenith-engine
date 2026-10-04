// The entry points through which the Zenith editor (or a game executable)
// loads {{PROJECT_NAME}}'s gameplay. ZENITH_GAME_MODULE defines the module's
// name, version, and compatibility checks; zenith_module_register adds the
// game's behaviours and systems. The input actions are the project's
// (input_actions.json, Edit > Project Settings > Input Actions).

#include "FpsGame.hpp"
#include "GoalZone.hpp"
#include "HitscanWeapon.hpp"
#include "Target.hpp"
// zenith:includes (the editor's code generators add their headers above)

#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"

ZENITH_GAME_MODULE("{{PROJECT_IDENTIFIER}}", "0.1.0");

void zenith_module_register(ModuleRegistrar& registrar) {
    registerFpsGame(registrar);
    registerHitscanWeapon(registrar);
    registerTarget(registrar);
    registerGoalZone(registrar);
    // zenith:register (the editor's code generators add their types above)
}
