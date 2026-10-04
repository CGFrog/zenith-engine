#pragma once

#include "engine/scene/ComponentTypes.hpp"

#include <string_view>

class ModuleRegistrar;

// The demo's gameplay as a game module registers it: component types the
// editor can author and the systems that simulate them. The zenith_game_demo
// module calls registerDemoGameplay from zenith_module_register, and the
// zenith executable calls it through its own ModuleRegistrar when it runs
// without a game module, so the editor and the game run identical
// registration.
namespace DemoGameplay {
inline constexpr std::string_view CharacterControllerKey = "character_controller";
inline constexpr std::string_view CollectibleItemKey = "collectible_item";
inline constexpr std::string_view ItemCollectorKey = "item_collector";
// Update, play mode: turns look input into controller yaw and pitch and
// orients each controller's camera.
inline constexpr std::string_view FirstPersonViewSystem = "demo.first_person_view";
// FixedUpdate, play mode: moves controller bodies, applies jumps, and tracks
// grounding. Needs the host's application services for physics.
inline constexpr std::string_view FirstPersonMovementSystem = "demo.first_person_movement";
// FixedUpdate, play mode: turns new collector/item sensor contacts into
// collection progress and removes collected item entities.
inline constexpr std::string_view ItemCollectionSystem = "demo.item_collection";
} // namespace DemoGameplay

// Describes CharacterControllerComponent for scene documents and the editor
// inspector.
ComponentType characterControllerComponentType();

// Registers every demo gameplay component type and system. The systems read
// the demo's input bindings from SystemContext::input and do nothing without
// it; failures are recorded by the registrar.
void registerDemoGameplay(ModuleRegistrar& registrar);
