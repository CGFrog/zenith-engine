// A project's input action file: source names, the JSON round trip, the
// errors a hand-edited file reports, and the map's read and override API.

#include "engine/input/InputActionsJson.hpp"

#include "support/Doctest.hpp"

#include <iostream>
#include <string>
#include <string_view>

TEST_SUITE_BEGIN("InputActionsJson");

namespace {

TEST_CASE("InputActionsJson: source names") {
    bool everyNameRoundTrips = true;
    for (const InputBindingSource& source : allInputSources()) {
        const std::optional<InputBindingSource> parsed = parseInputSource(inputSourceName(source));
        everyNameRoundTrips = everyNameRoundTrips && parsed && *parsed == source;
    }
    CHECK_MESSAGE(everyNameRoundTrips, "every input source's name parses back to it");
    CHECK_MESSAGE((inputSourceName(Key::LeftShift) == "key:LeftShift" &&
                   inputSourceName(MouseButton::Left) == "mouseButton:Left" &&
                   inputSourceName(GamepadAxis::RightTrigger) == "gamepadAxis:RightTrigger"),
                  "sources are named by kind and value");
    CHECK_MESSAGE((!parseInputSource("key:Tab") && !parseInputSource("keyboard:W") && !parseInputSource("W")),
                  "unknown keys, kinds, and names without a kind do not parse");
}

TEST_CASE("InputActionsJson: round trip") {
    InputActionMap actions;
    actions.addAction("Move", InputActionValueType::Axis2D, false);
    actions.addBinding("Move", {.source = Key::W, .scale = {0.0f, 1.0f}});
    actions.addBinding("Move", {.source = GamepadAxis::LeftX, .scale = {1.0f, 0.0f}, .deadZone = 0.25f});
    actions.addAction("Sprint", InputActionValueType::Button);
    actions.addBinding("Sprint", {.source = Key::LeftShift, .modifiers = {Key::LeftControl}});
    actions.addAction("Fire", InputActionValueType::Button);
    actions.addBinding("Fire", {.source = MouseButton::Left});

    const std::string text = serializeInputActionMap(actions);
    CHECK_MESSAGE((text.find("\"key:W\"") != std::string::npos && text.find("\"axis2d\"") != std::string::npos),
                  "the file names sources and types as text");
    InputActionMap loaded;
    CHECK_MESSAGE((static_cast<bool>(deserializeInputActionMap(text, loaded))), "the file reads back");
    CHECK_MESSAGE(serializeInputActionMap(loaded) == text,
                  "actions, bindings, scales, dead zones, and modifiers round-trip");
    CHECK_MESSAGE((loaded.actionType("Move") == InputActionValueType::Axis2D && !loaded.clampsValue("Move") &&
                   loaded.bindings("Move").size() == 2 && loaded.bindings("Move")[1].deadZone == 0.25f &&
                   loaded.bindings("Sprint")[0].modifiers == std::vector<Key>{Key::LeftControl}),
                  "the read API returns what the file declares");

    InputActionMap defaults;
    CHECK_MESSAGE((static_cast<bool>(deserializeInputActionMap(
                      R"({"version":1,"actions":[{"name":"Jump","bindings":[{"source":"key:Space"}]}]})", defaults))),
                  "a minimal file reads");
    CHECK_MESSAGE((defaults.actionType("Jump") == InputActionValueType::Button && defaults.clampsValue("Jump") &&
                   defaults.bindings("Jump")[0].scale == glm::vec2(1.0f, 0.0f) &&
                   defaults.bindings("Jump")[0].deadZone == 0.15f),
                  "omitted type, clamp, scale, and dead zone take their defaults");
}

TEST_CASE("InputActionsJson: errors") {
    InputActionMap kept;
    kept.addAction("Kept", InputActionValueType::Button);
    const auto failsWith = [&kept](std::string_view text, std::string_view fragment) {
        const InputActionsJsonResult result = deserializeInputActionMap(text, kept);
        return !result && result.error.find(fragment) != std::string::npos && kept.contains("Kept");
    };
    CHECK_MESSAGE(failsWith("not json", "JSON"), "a file that is not JSON fails");
    CHECK_MESSAGE(failsWith(R"({"version":9,"actions":[]})", "version"), "a newer version fails");
    CHECK_MESSAGE(
        failsWith(R"({"version":1,"actions":[{"name":"Jump","bindings":[{"source":"key:Tab"}]}]})", "key:Tab"),
        "an unknown source names itself");
    CHECK_MESSAGE(failsWith(R"({"version":1,"actions":[{"name":"Jump","type":"trigger"}]})", "Jump"),
                  "an unknown type names its action");
    CHECK_MESSAGE(failsWith(R"({"version":1,"actions":[{"name":"Jump"},{"name":"Jump"}]})", "twice"),
                  "an action declared twice fails");
    CHECK_MESSAGE(
        failsWith(
            R"({"version":1,"actions":[{"name":"Jump","bindings":[{"source":"key:W","modifiers":["mouseButton:Left"]}]}]})",
            "modifier"),
        "a modifier that is not a key fails");
}

TEST_CASE("InputActionsJson: override") {
    InputActionMap module;
    module.addAction("Jump", InputActionValueType::Button);
    module.addBinding("Jump", {.source = Key::Space});
    module.addAction("Look", InputActionValueType::Axis2D);
    InputActionMap project;
    project.addAction("Jump", InputActionValueType::Axis1D, false);
    project.addAction("Crouch", InputActionValueType::Button);
    module.overrideWith(project);
    CHECK_MESSAGE((module.actionNames() == std::vector<std::string>{"Crouch", "Jump", "Look"} &&
                   module.actionType("Jump") == InputActionValueType::Axis1D && module.bindings("Jump").empty()),
                  "an override replaces a shared action whole and adds new ones");
    CHECK_MESSAGE((module.removeAction("Look") && !module.contains("Look") && !module.removeAction("Look")),
                  "removeAction removes an action once");
}
} // namespace

TEST_SUITE_END();
