#pragma once

#include "engine/module/Gameplay.hpp"

// The demo gameplay statically linked into the game executable, registered
// through a ModuleRegistrar exactly as the zenith_game_demo module registers
// it: the built-in component types plus the gameplay's, and a scheduler for
// the gameplay's systems. The game plays it when no game module is loaded
// (see RuntimeGameplay). Leaves gameplay unchanged on failure.
GameplayResult linkDemoGameplay(Gameplay& gameplay);
