#pragma once

#include "engine/behaviour/Behaviour.hpp"

#include <string_view>

class GameServices;
class ModuleRegistrar;

// Something to shoot. It counts itself into the round when it starts, and a
// shot that empties its health destroys it (TargetDestroyed).
class Target final : public Behaviour {
  public:
    float health{50.0f};

    void onStart(BehaviourContext& context) override;

    // A shot dealt damage to self: publishes TargetHit, and once health runs
    // out TargetDestroyed and a destroy command.
    void takeDamage(GameServices& services, Scene& scene, Entity self, float damage);

  private:
    bool m_destroyed{false};
};

inline constexpr std::string_view TargetKey = "target";

void registerTarget(ModuleRegistrar& registrar);
