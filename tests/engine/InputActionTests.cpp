#include "engine/input/InputActions.hpp"
#include "engine/input/InputProfiles.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// What modules declare through ModuleRegistrar::addInputActions: maps merge
// into the host's, and a saved profile rebinds the merged defaults.
void testMergeAndProfiles() {
    InputActionMap host;
    InputActionMap first;
    first.addAction("Jump", InputActionValueType::Button);
    first.addBinding("Jump", {.source = Key::Space});
    InputActionMap second;
    second.addAction("Fire", InputActionValueType::Button);
    second.addBinding("Fire", {.source = MouseButton::Left});
    std::string error;
    expect(host.merge(first, error) && host.merge(second, error) &&
               host.actionNames() == std::vector<std::string>{"Fire", "Jump"} && host.bindingCount("Jump") == 1,
           "action maps merge with their bindings");
    InputActionMap clash;
    clash.addAction("Jump", InputActionValueType::Axis1D);
    clash.addAction("Crouch", InputActionValueType::Button);
    expect(!host.merge(clash, error) && error.find("Jump") != std::string::npos && !host.contains("Crouch"),
           "merging an action twice fails, names it, and adds nothing");

    InputProfile profile{.name = "Lefty",
                         .bindings = {{.action = "Jump", .binding = {.source = Key::RightControl}},
                                      {.action = "Jump", .binding = {.source = GamepadButton::South}}}};
    expect(applyInputProfile(profile, host).success && host.bindingCount("Jump") == 2 && host.bindingCount("Fire") == 1,
           "a profile replaces the bindings of the actions it names and keeps the others");
    InputState pressed;
    pressed.setKeyDown(Key::RightControl);
    pressed.setKeyPressed(Key::RightControl);
    InputState oldKey;
    oldKey.setKeyDown(Key::Space);
    oldKey.setKeyPressed(Key::Space);
    expect(host.evaluate(pressed).pressed("Jump") && !host.evaluate(oldKey).pressed("Jump"),
           "the profile's binding drives the action and the default no longer does");
    const InputProfile unknown{.name = "Broken", .bindings = {{.action = "Dance", .binding = {.source = Key::D}}}};
    expect(!applyInputProfile(unknown, host) && host.bindingCount("Jump") == 2,
           "a profile naming an unknown action fails and changes nothing");
}
} // namespace

int main() {
    InputActionMap actions;
    expect(actions.addAction("Move", InputActionValueType::Axis2D), "actions can be registered");
    expect(actions.addBinding("Move", {.source = Key::W, .scale = {0.0f, 1.0f}}),
           "keyboard bindings can be registered");
    expect(actions.addBinding("Move", {.source = Key::D, .scale = {1.0f, 0.0f}}),
           "multiple bindings can contribute to an action");
    expect(actions.addBinding("Move", {.source = GamepadAxis::LeftX, .scale = {1.0f, 0.0f}, .deadZone = 0.2f}),
           "gamepad axes can be registered with a dead zone");

    InputState diagonal;
    diagonal.setKeyDown(Key::W);
    diagonal.setKeyDown(Key::D);
    const glm::vec2 movement = actions.evaluate(diagonal).axis2D("Move");
    expect(std::abs(glm::length(movement) - 1.0f) < 0.0001f && movement.x > 0.0f && movement.y > 0.0f,
           "two-dimensional actions normalize diagonal digital input");

    InputState deadZone;
    deadZone.setGamepadAxis(GamepadAxis::LeftX, 0.15f);
    expect(actions.evaluate(deadZone).axis2D("Move").x == 0.0f, "gamepad values inside the dead zone are zero");
    deadZone.setGamepadAxis(GamepadAxis::LeftX, 0.6f);
    expect(std::abs(actions.evaluate(deadZone).axis2D("Move").x - 0.5f) < 0.0001f,
           "gamepad values outside the dead zone are rescaled continuously");

    expect(actions.addAction("Save", InputActionValueType::Button), "button actions can be registered");
    expect(actions.addBinding("Save", {.source = Key::S, .modifiers = {Key::LeftControl}}),
           "chord bindings can be registered");
    InputState chord;
    chord.setKeyDown(Key::LeftControl);
    chord.setKeyDown(Key::S);
    chord.setKeyPressed(Key::S);
    expect(actions.evaluate(chord).pressed("Save"), "a chord activates while all modifiers are held");
    chord.setKeyDown(Key::LeftControl, false);
    expect(!actions.evaluate(chord).pressed("Save"), "a chord does not activate without its modifier");

    expect(actions.addAction("Jump", InputActionValueType::Button), "jump action can be registered");
    expect(actions.addBinding("Jump", {.source = Key::Space}), "jump can be bound to a key");
    InputState edge;
    edge.setKeyReleased(Key::Space);
    expect(actions.evaluate(edge).released("Jump"), "release edges propagate through action evaluation");
    expect(actions.replaceBinding("Jump", 0, {.source = GamepadButton::South}), "an existing binding can be rebound");
    edge = {};
    edge.setGamepadConnected(true);
    edge.setGamepadButtonDown(GamepadButton::South);
    edge.setGamepadButtonPressed(GamepadButton::South);
    edge.setActiveDevice(InputDevice::Gamepad);
    const InputActionState rebound = actions.evaluate(edge);
    expect(rebound.pressed("Jump") && rebound.activeDevice() == InputDevice::Gamepad,
           "rebound gamepad input reports the action and active device");
    expect(actions.clearBindings("Jump") && actions.bindingCount("Jump") == 0,
           "bindings can be cleared for user configuration");

    InputState disconnected;
    disconnected.setGamepadConnected(false);
    disconnected.setGamepadButtonReleased(GamepadButton::South);
    expect(!disconnected.gamepadConnected && disconnected.gamepadButtonReleased(GamepadButton::South),
           "a disconnected gamepad can release held controls without leaving stuck input");

    InputState captured;
    captured.setKeyDown(Key::W);
    captured.setKeyPressed(Key::W);
    captured.setMouseButtonDown(MouseButton::Left);
    captured.mouseDelta = {5.0f, 7.0f};
    captured.applyCapture({.keyboard = true, .mouse = true});
    expect(!captured.keyDown(Key::W) && !captured.keyPressed(Key::W) && !captured.mouseButtonDown(MouseButton::Left) &&
               captured.mouseDelta == glm::vec2(0.0f),
           "UI capture suppresses keyboard and mouse state before action evaluation");

    InputEdgeBuffer fixedEdges;
    InputState renderFrame;
    renderFrame.setGamepadButtonPressed(GamepadButton::South);
    fixedEdges.accumulate(renderFrame);
    expect(fixedEdges.apply({}).gamepadButtonPressed(GamepadButton::South),
           "short press edges remain buffered until a fixed update consumes them");
    fixedEdges.consume();
    expect(!fixedEdges.apply({}).gamepadButtonPressed(GamepadButton::South),
           "fixed-update edge consumption prevents duplicate action presses");

    testMergeAndProfiles();
    return failures == 0 ? 0 : 1;
}
