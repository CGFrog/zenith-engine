// A project's input action file: source names, the JSON round trip, the
// errors a hand-edited file reports, and the map's read and override API.

#include "engine/input/InputActionsJson.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void testSourceNames() {
    bool everyNameRoundTrips = true;
    for (const InputBindingSource& source : allInputSources()) {
        const std::optional<InputBindingSource> parsed = parseInputSource(inputSourceName(source));
        everyNameRoundTrips = everyNameRoundTrips && parsed && *parsed == source;
    }
    expect(everyNameRoundTrips, "every input source's name parses back to it");
    expect(inputSourceName(Key::LeftShift) == "key:LeftShift" &&
               inputSourceName(MouseButton::Left) == "mouseButton:Left" &&
               inputSourceName(GamepadAxis::RightTrigger) == "gamepadAxis:RightTrigger",
           "sources are named by kind and value");
    expect(!parseInputSource("key:Tab") && !parseInputSource("keyboard:W") && !parseInputSource("W"),
           "unknown keys, kinds, and names without a kind do not parse");
}

void testRoundTrip() {
    InputActionMap actions;
    actions.addAction("Move", InputActionValueType::Axis2D, false);
    actions.addBinding("Move", {.source = Key::W, .scale = {0.0f, 1.0f}});
    actions.addBinding("Move", {.source = GamepadAxis::LeftX, .scale = {1.0f, 0.0f}, .deadZone = 0.25f});
    actions.addAction("Sprint", InputActionValueType::Button);
    actions.addBinding("Sprint", {.source = Key::LeftShift, .modifiers = {Key::LeftControl}});
    actions.addAction("Fire", InputActionValueType::Button);
    actions.addBinding("Fire", {.source = MouseButton::Left});

    const std::string text = serializeInputActionMap(actions);
    expect(text.find("\"key:W\"") != std::string::npos && text.find("\"axis2d\"") != std::string::npos,
           "the file names sources and types as text");
    InputActionMap loaded;
    expect(static_cast<bool>(deserializeInputActionMap(text, loaded)), "the file reads back");
    expect(serializeInputActionMap(loaded) == text, "actions, bindings, scales, dead zones, and modifiers round-trip");
    expect(loaded.actionType("Move") == InputActionValueType::Axis2D && !loaded.clampsValue("Move") &&
               loaded.bindings("Move").size() == 2 && loaded.bindings("Move")[1].deadZone == 0.25f &&
               loaded.bindings("Sprint")[0].modifiers == std::vector<Key>{Key::LeftControl},
           "the read API returns what the file declares");

    InputActionMap defaults;
    expect(static_cast<bool>(deserializeInputActionMap(
               R"({"version":1,"actions":[{"name":"Jump","bindings":[{"source":"key:Space"}]}]})", defaults)),
           "a minimal file reads");
    expect(defaults.actionType("Jump") == InputActionValueType::Button && defaults.clampsValue("Jump") &&
               defaults.bindings("Jump")[0].scale == glm::vec2(1.0f, 0.0f) &&
               defaults.bindings("Jump")[0].deadZone == 0.15f,
           "omitted type, clamp, scale, and dead zone take their defaults");
}

void testErrors() {
    InputActionMap kept;
    kept.addAction("Kept", InputActionValueType::Button);
    const auto failsWith = [&kept](std::string_view text, std::string_view fragment) {
        const InputActionsJsonResult result = deserializeInputActionMap(text, kept);
        return !result && result.error.find(fragment) != std::string::npos && kept.contains("Kept");
    };
    expect(failsWith("not json", "JSON"), "a file that is not JSON fails");
    expect(failsWith(R"({"version":9,"actions":[]})", "version"), "a newer version fails");
    expect(failsWith(R"({"version":1,"actions":[{"name":"Jump","bindings":[{"source":"key:Tab"}]}]})", "key:Tab"),
           "an unknown source names itself");
    expect(failsWith(R"({"version":1,"actions":[{"name":"Jump","type":"trigger"}]})", "Jump"),
           "an unknown type names its action");
    expect(failsWith(R"({"version":1,"actions":[{"name":"Jump"},{"name":"Jump"}]})", "twice"),
           "an action declared twice fails");
    expect(
        failsWith(
            R"({"version":1,"actions":[{"name":"Jump","bindings":[{"source":"key:W","modifiers":["mouseButton:Left"]}]}]})",
            "modifier"),
        "a modifier that is not a key fails");
}

void testOverride() {
    InputActionMap module;
    module.addAction("Jump", InputActionValueType::Button);
    module.addBinding("Jump", {.source = Key::Space});
    module.addAction("Look", InputActionValueType::Axis2D);
    InputActionMap project;
    project.addAction("Jump", InputActionValueType::Axis1D, false);
    project.addAction("Crouch", InputActionValueType::Button);
    module.overrideWith(project);
    expect(module.actionNames() == std::vector<std::string>{"Crouch", "Jump", "Look"} &&
               module.actionType("Jump") == InputActionValueType::Axis1D && module.bindings("Jump").empty(),
           "an override replaces a shared action whole and adds new ones");
    expect(module.removeAction("Look") && !module.contains("Look") && !module.removeAction("Look"),
           "removeAction removes an action once");
}
} // namespace

int main() {
    testSourceNames();
    testRoundTrip();
    testErrors();
    testOverride();
    if (failures != 0) {
        std::cerr << failures << " input actions JSON test(s) failed\n";
        return 1;
    }
    std::cout << "Input actions JSON tests passed\n";
    return 0;
}
