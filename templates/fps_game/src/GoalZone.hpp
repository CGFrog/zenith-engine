#pragma once

#include "engine/behaviour/Behaviour.hpp"

#include <string_view>

class ModuleRegistrar;

// The exit: put it on a sensor (a rigid body with sensor set). When a
// character walks in, it publishes GoalReached once.
class GoalZone final : public Behaviour {
  public:
    void onTriggerEnter(BehaviourContext& context, Entity other) override;

  private:
    bool m_reached{false};
};

inline constexpr std::string_view GoalZoneKey = "goal_zone";

void registerGoalZone(ModuleRegistrar& registrar);
