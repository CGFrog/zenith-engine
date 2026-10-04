#include "HitscanWeapon.hpp"

#include "FpsGame.hpp"
#include "Target.hpp"

#include "engine/module/GameServices.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/physics/PhysicsWorld.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>

void HitscanWeapon::onUpdate(BehaviourContext& context) {
    m_cooldownLeft = std::max(0.0f, m_cooldownLeft - context.deltaSeconds);
    if (context.services == nullptr || context.physics == nullptr || context.actions == nullptr) {
        return;
    }
    Scene& scene = context.scene;
    if (fpsGameState(scene).screen != FpsScreen::Playing || m_cooldownLeft > 0.0f ||
        !context.actions->pressed(fireAction)) {
        return;
    }
    m_cooldownLeft = cooldownSeconds;

    // The camera looks down its -Z axis. The ray ignores the camera and the
    // character carrying it.
    const glm::mat4& world = scene.registry().get<TransformComponent>(context.entity).world;
    const glm::vec3 origin(world[3]);
    const glm::vec3 forward = -glm::normalize(glm::vec3(world[2]));
    const auto* relationship = scene.registry().try_get<RelationshipComponent>(context.entity);
    const std::array<Entity, 2> ignored{context.entity, relationship != nullptr ? relationship->parent : entt::null};
    PhysicsRayHit hit;
    if (!context.physics->raycast(origin, forward, range, hit, {.ignore = ignored})) {
        return;
    }
    if (!impactEffect.empty()) {
        context.services->effects().spawn(impactEffect, glm::translate(glm::mat4(1.0f), hit.position),
                                          {.pooled = true});
    }
    if (Target* target = scene.registry().try_get<Target>(hit.entity)) {
        target->takeDamage(*context.services, scene, hit.entity, damage);
    }
}

void registerHitscanWeapon(ModuleRegistrar& registrar) {
    registrar.addBehaviour<HitscanWeapon>(
        std::string(HitscanWeaponKey), "Hitscan Weapon",
        std::move(Fields<HitscanWeapon>()
                      .add("range", &HitscanWeapon::range)
                      .drag(1.0f, 1.0, 1000.0)
                      .format("%.0f m")
                      .add("damage", &HitscanWeapon::damage)
                      .drag(1.0f, 0.0, 1000.0)
                      .add("cooldown_seconds", &HitscanWeapon::cooldownSeconds)
                      .drag(0.01f, 0.0, 5.0)
                      .format("%.2f s")
                      .add("fire_action", &HitscanWeapon::fireAction)
                      .tooltip("The button input action that fires")
                      .add("impact_effect", &HitscanWeapon::impactEffect)
                      .tooltip("A prefab spawned where a shot hits; empty for none")));
}
