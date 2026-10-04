#include "GoalZone.hpp"

#include "FpsGame.hpp"

#include "engine/module/GameServices.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/physics/CharacterController.hpp"

#include <string>

void GoalZone::onTriggerEnter(BehaviourContext& context, Entity other) {
    if (m_reached || context.services == nullptr ||
        !context.scene.registry().all_of<CharacterControllerComponent>(other)) {
        return;
    }
    m_reached = true;
    context.services->events().publish(GoalReached{});
}

void registerGoalZone(ModuleRegistrar& registrar) {
    registrar.addBehaviour<GoalZone>(std::string(GoalZoneKey), "Goal Zone");
}
