#pragma once

#include "BehaviourRegistry.hpp"

class GameServices;
class Logger;

// Runs the behaviours of context.behaviours for one phase (Update,
// FixedUpdate, or LateUpdate): first the lifecycle hooks every behaviour is
// due (onCreate, onEnable/onDisable, then onStart), then, in FixedUpdate, the
// trigger and collision hooks of the physics step since the last one
// (GameServices::takePhysicsEvents), then the phase's update hook. Types run
// in registration order and, within a type, entities in creation order (scene
// id). Only types whose RunIn admits context.mode run. The engine's built-in
// behaviour systems call it; it does nothing without behaviours or in the Ui
// phase.
void runBehaviours(SystemPhase phase, const SystemContext& context);

// Calls onDisable (when enabled) and onDestroy on every created behaviour of
// root and its descendants, parents first, before they are destroyed.
// GameServices calls it for entities its EntityCommandBuffer destroys.
void destroyBehaviours(const BehaviourRegistry& behaviours, Scene& scene, Entity root, GameServices* services,
                       Logger* logger);
