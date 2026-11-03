#include "InputState.hpp"

#include <cstddef>

namespace {
size_t keyIndex(Key key) {
    return static_cast<size_t>(key);
}

size_t mouseButtonIndex(MouseButton button) {
    return static_cast<size_t>(button);
}

size_t gamepadButtonIndex(GamepadButton button) {
    return static_cast<size_t>(button);
}

size_t gamepadAxisIndex(GamepadAxis axis) {
    return static_cast<size_t>(axis);
}
} // namespace

bool InputState::keyDown(Key key) const {
    return m_down.test(keyIndex(key));
}

bool InputState::keyPressed(Key key) const {
    return m_pressed.test(keyIndex(key));
}

bool InputState::keyReleased(Key key) const {
    return m_released.test(keyIndex(key));
}

void InputState::setKeyDown(Key key, bool down) {
    m_down.set(keyIndex(key), down);
}

void InputState::setKeyPressed(Key key, bool pressed) {
    m_pressed.set(keyIndex(key), pressed);
}

void InputState::setKeyReleased(Key key, bool released) {
    m_released.set(keyIndex(key), released);
}

bool InputState::mouseButtonDown(MouseButton button) const {
    return m_mouseDown.test(mouseButtonIndex(button));
}

bool InputState::mouseButtonPressed(MouseButton button) const {
    return m_mousePressed.test(mouseButtonIndex(button));
}

bool InputState::mouseButtonReleased(MouseButton button) const {
    return m_mouseReleased.test(mouseButtonIndex(button));
}

void InputState::setMouseButtonDown(MouseButton button, bool down) {
    m_mouseDown.set(mouseButtonIndex(button), down);
}

void InputState::setMouseButtonPressed(MouseButton button, bool pressed) {
    m_mousePressed.set(mouseButtonIndex(button), pressed);
}

void InputState::setMouseButtonReleased(MouseButton button, bool released) {
    m_mouseReleased.set(mouseButtonIndex(button), released);
}

bool InputState::gamepadButtonDown(GamepadButton button) const {
    return m_gamepadDown.test(gamepadButtonIndex(button));
}

bool InputState::gamepadButtonPressed(GamepadButton button) const {
    return m_gamepadPressed.test(gamepadButtonIndex(button));
}

bool InputState::gamepadButtonReleased(GamepadButton button) const {
    return m_gamepadReleased.test(gamepadButtonIndex(button));
}

void InputState::setGamepadButtonDown(GamepadButton button, bool down) {
    m_gamepadDown.set(gamepadButtonIndex(button), down);
}

void InputState::setGamepadButtonPressed(GamepadButton button, bool pressed) {
    m_gamepadPressed.set(gamepadButtonIndex(button), pressed);
}

void InputState::setGamepadButtonReleased(GamepadButton button, bool released) {
    m_gamepadReleased.set(gamepadButtonIndex(button), released);
}

float InputState::gamepadAxis(GamepadAxis axis) const {
    return m_gamepadAxes[gamepadAxisIndex(axis)];
}

void InputState::setGamepadAxis(GamepadAxis axis, float value) {
    m_gamepadAxes[gamepadAxisIndex(axis)] = std::clamp(value, -1.0f, 1.0f);
}

void InputState::setGamepadConnected(bool connected) {
    gamepadConnected = connected;
}

void InputState::setActiveDevice(InputDevice device) {
    activeDevice = device;
}

void InputState::applyCapture(InputCapture capture) {
    if (capture.keyboard) {
        m_down.reset();
        m_pressed.reset();
        m_released.reset();
    }
    if (capture.mouse) {
        m_mouseDown.reset();
        m_mousePressed.reset();
        m_mouseReleased.reset();
        mouseDelta = {};
        mouseWheelDelta = {};
    }
}

void InputEdgeBuffer::accumulate(const InputState& state) {
    m_keyPresses |= state.m_pressed;
    m_keyReleases |= state.m_released;
    m_mousePresses |= state.m_mousePressed;
    m_mouseReleases |= state.m_mouseReleased;
    m_gamepadPresses |= state.m_gamepadPressed;
    m_gamepadReleases |= state.m_gamepadReleased;
    m_mouseWheelDelta += state.mouseWheelDelta;
}

InputState InputEdgeBuffer::apply(const InputState& state) const {
    InputState buffered = state;
    buffered.m_pressed = m_keyPresses;
    buffered.m_released = m_keyReleases;
    buffered.m_mousePressed = m_mousePresses;
    buffered.m_mouseReleased = m_mouseReleases;
    buffered.m_gamepadPressed = m_gamepadPresses;
    buffered.m_gamepadReleased = m_gamepadReleases;
    buffered.mouseWheelDelta = m_mouseWheelDelta;
    // Drops belong to the frame, not to each fixed step.
    buffered.droppedFiles.clear();
    return buffered;
}

void InputEdgeBuffer::consume() {
    clear();
}

void InputEdgeBuffer::clear() {
    m_keyPresses.reset();
    m_keyReleases.reset();
    m_mousePresses.reset();
    m_mouseReleases.reset();
    m_gamepadPresses.reset();
    m_gamepadReleases.reset();
    m_mouseWheelDelta = {};
}
