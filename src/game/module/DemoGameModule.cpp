// The zenith_game_demo shared library: the entry points through which the
// editor, and the zenith executable for the demo project, load the demo's
// gameplay. The executable also links the same gameplay statically, for runs
// without a game module, and calls registerDemoGameplay directly.

#include "engine/module/ModuleApi.hpp"
#include "game/gameplay/DemoGameplay.hpp"

ZENITH_GAME_MODULE("demo", "1.0.0");

void zenith_module_register(ModuleRegistrar& registrar) {
    registerDemoGameplay(registrar);
}
