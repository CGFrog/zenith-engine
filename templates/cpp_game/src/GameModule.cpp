// The entry points through which the Zenith editor (or a game executable)
// loads {{PROJECT_NAME}}'s gameplay. ZENITH_GAME_MODULE defines the module's
// name, version, and compatibility checks; zenith_module_register adds the
// game's component types and systems.

#include "SpinnerGameplay.hpp"

#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"

ZENITH_GAME_MODULE("{{PROJECT_IDENTIFIER}}", "0.1.0");

void zenith_module_register(ModuleRegistrar& registrar) {
    registerSpinnerGameplay(registrar);
}
