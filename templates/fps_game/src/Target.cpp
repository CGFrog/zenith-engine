#include "Target.hpp"

#include "FpsGame.hpp"

#include "engine/module/GameServices.hpp"
#include "engine/module/ModuleRegistrar.hpp"

#include <string>

void Target::onStart(BehaviourContext& context) {
    ++fpsGameState(context.scene).targetsTotal;
}

void Target::takeDamage(GameServices& services, Scene& scene, Entity self, float damage) {
    if (m_destroyed) {
        return;
    }
    health -= damage;
    const SceneEntityId id = scene.id(self);
    services.events().publish(TargetHit{.target = id, .healthLeft = health});
    if (health <= 0.0f) {
        m_destroyed = true;
        services.events().publish(TargetDestroyed{.target = id});
        services.commands().destroy(id);
    }
}

void registerTarget(ModuleRegistrar& registrar) {
    registrar.addBehaviour<Target>(std::string(TargetKey), "Target",
                                   std::move(Fields<Target>()
                                                 .add("health", &Target::health)
                                                 .drag(1.0f, 1.0, 1000.0)
                                                 .tooltip("Damage the target takes before it is destroyed")));
}
