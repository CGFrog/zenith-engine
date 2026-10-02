#include "DemoGameplay.hpp"

#include "engine/application/Application.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "game/input/DemoInput.hpp"
#include "game/player/FirstPersonController.hpp"

#include <memory>
#include <string>

namespace {
std::string validateCharacterController(const CharacterControllerComponent& controller) {
    if (!(controller.radius > 0.0f)) {
        return "character controller radius must be positive";
    }
    if (!(controller.halfHeight >= 0.0f)) {
        return "character controller half height must not be negative";
    }
    if (!(controller.moveSpeed >= 0.0f) || !(controller.sprintMultiplier >= 0.0f) || !(controller.jumpSpeed >= 0.0f) ||
        !(controller.acceleration >= 0.0f)) {
        return "character controller speeds and acceleration must not be negative";
    }
    if (!(controller.airControl >= 0.0f && controller.airControl <= 1.0f)) {
        return "character controller air control must be between 0 and 1";
    }
    return {};
}

FirstPersonViewPreferences viewPreferences(const Scene& scene) {
    const auto* preferences = scene.registry().ctx().find<FirstPersonViewPreferences>();
    return preferences ? *preferences : FirstPersonViewPreferences{};
}

// The systems share one immutable binding map; evaluating it is stateless.
SystemDescriptor firstPersonViewSystem(std::shared_ptr<const InputActionMap> bindings) {
    return {.name = std::string(DemoGameplay::FirstPersonViewSystem),
            .phase = SystemPhase::Update,
            .runIn = RunIn::Play,
            .run = [bindings = std::move(bindings)](const SystemContext& context) {
                if (context.input == nullptr) {
                    return;
                }
                const InputActionState actions = bindings->evaluate(*context.input);
                const FirstPersonViewPreferences preferences = viewPreferences(context.scene);
                glm::vec2 look = actions.axis2D(DemoAction::Look) * preferences.lookScale;
                look.y *= preferences.invertLookY ? -1.0f : 1.0f;
                FirstPersonControllerSystem{}.updateView(
                    context.scene, {.look = look, .gamepadLook = actions.activeDevice() == InputDevice::Gamepad},
                    context.deltaSeconds);
            }};
}

SystemDescriptor firstPersonMovementSystem(std::shared_ptr<const InputActionMap> bindings) {
    return {.name = std::string(DemoGameplay::FirstPersonMovementSystem),
            .phase = SystemPhase::FixedUpdate,
            .runIn = RunIn::Play,
            .run = [bindings = std::move(bindings)](const SystemContext& context) {
                if (context.input == nullptr || context.application == nullptr) {
                    return;
                }
                const InputActionState actions = bindings->evaluate(*context.input);
                FirstPersonControllerSystem{}.fixedUpdate(context.scene, context.application->physics,
                                                          {.move = actions.axis2D(DemoAction::Move),
                                                           .jumpPressed = actions.pressed(DemoAction::Jump),
                                                           .sprint = actions.down(DemoAction::Sprint)},
                                                          context.deltaSeconds);
            }};
}
} // namespace

ComponentType characterControllerComponentType() {
    using Controller = CharacterControllerComponent;
    return ComponentTypeBuilder<Controller>(std::string(DemoGameplay::CharacterControllerKey), "Character Controller")
        .category("Gameplay")
        .fields(Fields<Controller>()
                    .add("radius", &Controller::radius)
                    .drag(0.01f, 0.01, 10.0)
                    .format("%.2f m")
                    .tooltip("Capsule radius; match the entity's capsule collider")
                    .add("half_height", &Controller::halfHeight)
                    .drag(0.01f, 0.0, 10.0)
                    .format("%.2f m")
                    .tooltip("Half the capsule's cylinder height; the camera sits at half height + radius")
                    .add("move_speed", &Controller::moveSpeed)
                    .drag(0.05f, 0.0, 100.0)
                    .format("%.2f m/s")
                    .add("sprint_multiplier", &Controller::sprintMultiplier)
                    .drag(0.01f, 0.0, 10.0)
                    .format("%.2fx")
                    .add("jump_speed", &Controller::jumpSpeed)
                    .drag(0.05f, 0.0, 100.0)
                    .format("%.2f m/s")
                    .tooltip("Upward speed at the start of a jump")
                    .add("acceleration", &Controller::acceleration)
                    .drag(0.1f, 0.0, 500.0)
                    .format("%.1f /s")
                    .tooltip("How quickly horizontal velocity approaches the requested velocity")
                    .add("air_control", &Controller::airControl)
                    .slider(0.0, 1.0)
                    .tooltip("Fraction of acceleration available while airborne")
                    .add("look_sensitivity", &Controller::lookSensitivity)
                    .drag(0.0001f, 0.0, 0.1)
                    .format("%.4f")
                    .tooltip("Radians of rotation per unit of look input")
                    .add("yaw", &Controller::yawRadians)
                    .angle()
                    .label("Initial yaw")
                    .add("pitch", &Controller::pitchRadians)
                    .angle()
                    .slider(-89.0, 89.0)
                    .label("Initial pitch"))
        .validate(validateCharacterController)
        .build();
}

void registerDemoGameplay(ModuleRegistrar& registrar) {
    registrar.addComponentType(characterControllerComponentType());

    const std::shared_ptr<const InputActionMap> bindings = createDemoInputMap();
    registrar.addSystem(firstPersonViewSystem(bindings));
    registrar.addSystem(firstPersonMovementSystem(bindings));
}
