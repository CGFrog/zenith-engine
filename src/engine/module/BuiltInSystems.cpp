#include "BuiltInSystems.hpp"

#include "GameServices.hpp"
#include "engine/animation/Animator.hpp"
#include "engine/application/Application.hpp"
#include "engine/behaviour/BehaviourScheduler.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "engine/navigation/NavAgent.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace {
SystemDescriptor characterInputSystem() {
    return {.name = std::string(EngineSystems::CharacterInput),
            .phase = SystemPhase::FixedUpdate,
            .runIn = RunIn::Play,
            .run = [](const SystemContext& context) {
                // Without player input (Simulate) characters keep their intent.
                if (context.actions != nullptr) {
                    applyCharacterInput(context.scene, *context.actions);
                }
            }};
}

SystemDescriptor firstPersonCameraSystem() {
    return {.name = std::string(EngineSystems::FirstPersonCamera),
            .phase = SystemPhase::LateUpdate,
            .runIn = RunIn::Play,
            .run = [](const SystemContext& context) {
                // A paused game holds the view still, as it does the player.
                const bool paused = context.services != nullptr && context.services->paused();
                updateFirstPersonCameras(context.scene, paused ? nullptr : context.actions, context.deltaSeconds);
            }};
}

SystemDescriptor behaviourSystem(std::string_view name, SystemPhase phase) {
    return {.name = std::string(name),
            .phase = phase,
            .runIn = RunIn::EditAndPlay,
            .run = [phase](const SystemContext& context) { runBehaviours(phase, context); }};
}

SystemDescriptor navAgentSystem() {
    return {.name = std::string(EngineSystems::NavAgents),
            .phase = SystemPhase::Update,
            .runIn = RunIn::Play,
            .run = [](const SystemContext& context) {
                if (context.services == nullptr || context.scene.registry().view<NavAgentComponent>().empty()) {
                    return;
                }
                updateNavAgents(context.scene, context.services->navigation(context.scene), context.deltaSeconds);
            }};
}

// Needs the application's assets for the models' clips.
SystemDescriptor animatorSystem() {
    return {.name = std::string(EngineSystems::Animator),
            .phase = SystemPhase::Update,
            .runIn = RunIn::Play,
            .order = EngineSystems::animatorOrder,
            .run = [](const SystemContext& context) {
                if (context.application != nullptr) {
                    updateAnimators(context.scene, context.application->assets,
                                    context.services != nullptr ? &context.services->events() : nullptr,
                                    context.deltaSeconds);
                }
            }};
}
} // namespace

SystemRegistryResult registerBuiltInSystems(SystemRegistry& systems) {
    for (SystemDescriptor system : {characterInputSystem(), firstPersonCameraSystem(),
                                    behaviourSystem(EngineSystems::Behaviours, SystemPhase::Update),
                                    behaviourSystem(EngineSystems::FixedBehaviours, SystemPhase::FixedUpdate),
                                    behaviourSystem(EngineSystems::LateBehaviours, SystemPhase::LateUpdate),
                                    navAgentSystem(), animatorSystem()}) {
        if (SystemRegistryResult result = systems.add(std::move(system)); !result) {
            return result;
        }
    }
    return {.success = true};
}

std::shared_ptr<const SystemRegistry> builtInSystems() {
    static const std::shared_ptr<const SystemRegistry> systems = [] {
        auto registry = std::make_shared<SystemRegistry>();
        // Fixed at compile time and covered by tests, so a failure is a
        // programming error.
        if (const SystemRegistryResult result = registerBuiltInSystems(*registry); !result) {
            throw std::logic_error("Invalid built-in systems: " + result.error);
        }
        return std::shared_ptr<const SystemRegistry>(std::move(registry));
    }();
    return systems;
}
