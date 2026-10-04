#pragma once

#include "SystemScheduler.hpp"

#include <memory>
#include <string_view>

// The engine's own systems, which every Gameplay runs before a game's
// systems of the same phase and order. They drive the engine's reusable
// gameplay components and do nothing in a scene without them.
namespace EngineSystems {
// FixedUpdate, Play: walks player-driven characters from their move, jump
// and sprint actions (applyCharacterInput).
inline constexpr std::string_view CharacterInput = "zenith.character_input";
// LateUpdate, Play: turns first-person cameras by their look actions
// (updateFirstPersonCameras), after physics has placed their characters.
inline constexpr std::string_view FirstPersonCamera = "zenith.first_person_camera";
// Update, FixedUpdate, and LateUpdate, in Edit and Play: run the gameplay's
// behaviours (runBehaviours); each behaviour type's RunIn chooses its modes.
inline constexpr std::string_view Behaviours = "zenith.behaviours";
inline constexpr std::string_view FixedBehaviours = "zenith.fixed_behaviours";
inline constexpr std::string_view LateBehaviours = "zenith.late_behaviours";
// Update, Play: plans and follows the navigation agents' paths on the
// session's navigation mesh (GameServices::navigation, baked on first use).
inline constexpr std::string_view NavAgents = "zenith.nav_agents";
// Update, Play, after the game's Update systems (order animatorOrder): runs
// the animators (updateAnimators) before the animations advance.
inline constexpr std::string_view Animator = "zenith.animator";
inline constexpr int32_t animatorOrder = 1000;
} // namespace EngineSystems

// Adds the engine's systems to systems; fails if a name is taken.
SystemRegistryResult registerBuiltInSystems(SystemRegistry& systems);

// A registry holding only the engine's systems, shared and never changed.
std::shared_ptr<const SystemRegistry> builtInSystems();
