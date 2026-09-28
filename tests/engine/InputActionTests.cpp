#include "engine/input/InputActions.hpp"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
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

    return failures == 0 ? 0 : 1;
}
