#pragma once

#include "engine/behaviour/Behaviour.hpp"

#include <string>
#include <string_view>

class ModuleRegistrar;

// A hitscan gun on the player's camera: the fire action casts a ray along the
// camera's forward axis, ignoring the player, spawns an impact effect where
// it hits, and damages a Target there.
class HitscanWeapon final : public Behaviour {
  public:
    float range{100.0f};
    float damage{25.0f};
    float cooldownSeconds{0.2f};
    std::string fireAction{"Fire"};
    // A prefab relative to the asset root, spawned at each hit; empty for none.
    std::string impactEffect{"prefabs/impact.prefab"};

    void onUpdate(BehaviourContext& context) override;

  private:
    float m_cooldownLeft{0.0f};
};

inline constexpr std::string_view HitscanWeaponKey = "hitscan_weapon";

void registerHitscanWeapon(ModuleRegistrar& registrar);
