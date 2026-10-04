// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added.

#include "GameplayComponentTypes.hpp"

#include "ComponentTypes.hpp"
#include "Tags.hpp"
#include "engine/gameplay/FirstPerson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

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

ComponentType tagsComponentType() {
    using Json = nlohmann::json;
    return ComponentTypeBuilder<TagsComponent>("tags", "Tags")
        .category("Gameplay")
        .codec(
            [](const TagsComponent& component, const ComponentSaveContext&, Json& output) {
                output = {{"tags", component.tags}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, TagsComponent& component, const ComponentLoadContext&) {
                const auto tags = input.find("tags");
                if (tags == input.end() || !tags->is_array() ||
                    !std::ranges::all_of(*tags, [](const Json& tag) { return tag.is_string(); })) {
                    return SceneDocumentResult{.error = "tags must be an array of names"};
                }
                component.tags = tags->get<std::vector<std::string>>();
                return SceneDocumentResult{.success = true};
            })
        .validate([](const TagsComponent& component) {
            return std::ranges::any_of(component.tags, [](const std::string& tag) { return tag.empty(); })
                       ? std::string("a tag must not be empty")
                       : std::string{};
        })
        .build();
}
