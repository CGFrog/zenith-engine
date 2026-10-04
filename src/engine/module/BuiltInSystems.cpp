#include "BuiltInSystems.hpp"

#include "engine/gameplay/FirstPerson.hpp"

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
                updateFirstPersonCameras(context.scene, context.actions, context.deltaSeconds);
            }};
}
} // namespace

SystemRegistryResult registerBuiltInSystems(SystemRegistry& systems) {
    for (SystemDescriptor system : {characterInputSystem(), firstPersonCameraSystem()}) {
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
