// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added.

#include "GameplayComponentTypes.hpp"

#include "ComponentTypes.hpp"
#include "engine/gameplay/FirstPerson.hpp"

#include <string>

ComponentType firstPersonCameraComponentType() {
    using C = FirstPersonCameraComponent;
    return ComponentTypeBuilder<C>("first_person_camera", "First Person Camera")
        .category("Gameplay")
        .fields(Fields<C>()
                    .add("sensitivity", &C::sensitivity)
                    .drag(0.0001f, 0.0, 0.1)
                    .format("%.4f")
                    .tooltip("Radians of rotation per unit of look input")
                    .add("gamepad_look_speed", &C::gamepadLookSpeed)
                    .drag(1.0f, 0.0, 10000.0)
                    .tooltip("Look input per second from a gamepad stick at full deflection")
                    .add("minimum_pitch", &C::minimumPitchRadians)
                    .angle()
                    .slider(-89.0, 0.0)
                    .add("maximum_pitch", &C::maximumPitchRadians)
                    .angle()
                    .slider(0.0, 89.0)
                    .add("yaw", &C::yawRadians)
                    .angle()
                    .label("Initial yaw")
                    .add("pitch", &C::pitchRadians)
                    .angle()
                    .slider(-89.0, 89.0)
                    .label("Initial pitch")
                    .add("look_action", &C::lookAction)
                    .tooltip("2D input action that turns the view; empty leaves it to code"))
        .validate([](const C& view) -> std::string {
            if (!(view.sensitivity >= 0.0f) || !(view.gamepadLookSpeed >= 0.0f)) {
                return "first-person camera sensitivity and gamepad look speed must not be negative";
            }
            if (!(view.minimumPitchRadians <= view.maximumPitchRadians)) {
                return "first-person camera minimum pitch must not exceed its maximum";
            }
            return {};
        })
        .build();
}
