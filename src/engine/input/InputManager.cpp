#include "InputManager.hpp"

#include "../window/Window.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <optional>

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

std::optional<Key> keyForScancode(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_W:
        return Key::W;
    case SDL_SCANCODE_A:
        return Key::A;
    case SDL_SCANCODE_S:
        return Key::S;
    case SDL_SCANCODE_D:
        return Key::D;
    case SDL_SCANCODE_Q:
        return Key::Q;
    case SDL_SCANCODE_E:
        return Key::E;
    case SDL_SCANCODE_LSHIFT:
        return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT:
        return Key::RightShift;
    case SDL_SCANCODE_LCTRL:
        return Key::LeftControl;
    case SDL_SCANCODE_RCTRL:
        return Key::RightControl;
    case SDL_SCANCODE_SPACE:
        return Key::Space;
    case SDL_SCANCODE_F:
        return Key::F;
    case SDL_SCANCODE_R:
        return Key::R;
    case SDL_SCANCODE_ESCAPE:
        return Key::Escape;
    default:
        return std::nullopt;
    }
}

std::optional<MouseButton> mouseButtonForSdlButton(uint8_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
    case SDL_BUTTON_X1:
        return MouseButton::X1;
    case SDL_BUTTON_X2:
        return MouseButton::X2;
    default:
        return std::nullopt;
    }
}

SDL_MouseButtonFlags mouseButtonMask(MouseButton button) {
    switch (button) {
    case MouseButton::Left:
        return SDL_BUTTON_LMASK;
    case MouseButton::Middle:
        return SDL_BUTTON_MMASK;
    case MouseButton::Right:
        return SDL_BUTTON_RMASK;
    case MouseButton::X1:
        return SDL_BUTTON_X1MASK;
    case MouseButton::X2:
        return SDL_BUTTON_X2MASK;
    case MouseButton::Count:
        return 0;
    }
    return 0;
}

SDL_GamepadButton sdlGamepadButton(GamepadButton button) {
    switch (button) {
    case GamepadButton::South:
        return SDL_GAMEPAD_BUTTON_SOUTH;
    case GamepadButton::East:
        return SDL_GAMEPAD_BUTTON_EAST;
    case GamepadButton::West:
        return SDL_GAMEPAD_BUTTON_WEST;
    case GamepadButton::North:
        return SDL_GAMEPAD_BUTTON_NORTH;
    case GamepadButton::Back:
        return SDL_GAMEPAD_BUTTON_BACK;
    case GamepadButton::Guide:
        return SDL_GAMEPAD_BUTTON_GUIDE;
    case GamepadButton::Start:
        return SDL_GAMEPAD_BUTTON_START;
    case GamepadButton::LeftStick:
        return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case GamepadButton::RightStick:
        return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case GamepadButton::LeftShoulder:
        return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case GamepadButton::RightShoulder:
        return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case GamepadButton::DpadUp:
        return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case GamepadButton::DpadDown:
        return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case GamepadButton::DpadLeft:
        return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case GamepadButton::DpadRight:
        return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    case GamepadButton::Count:
        return SDL_GAMEPAD_BUTTON_INVALID;
    }
    return SDL_GAMEPAD_BUTTON_INVALID;
}

SDL_GamepadAxis sdlGamepadAxis(GamepadAxis axis) {
    switch (axis) {
    case GamepadAxis::LeftX:
        return SDL_GAMEPAD_AXIS_LEFTX;
    case GamepadAxis::LeftY:
        return SDL_GAMEPAD_AXIS_LEFTY;
    case GamepadAxis::RightX:
        return SDL_GAMEPAD_AXIS_RIGHTX;
    case GamepadAxis::RightY:
        return SDL_GAMEPAD_AXIS_RIGHTY;
    case GamepadAxis::LeftTrigger:
        return SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    case GamepadAxis::RightTrigger:
        return SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    case GamepadAxis::Count:
        return SDL_GAMEPAD_AXIS_INVALID;
    }
    return SDL_GAMEPAD_AXIS_INVALID;
}

float normalizeGamepadAxis(Sint16 value, bool trigger) {
    if (trigger) {
        return std::clamp(static_cast<float>(value) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX), 0.0f, 1.0f);
    }
    const float divisor =
        value < 0 ? -static_cast<float>(SDL_JOYSTICK_AXIS_MIN) : static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
    return std::clamp(static_cast<float>(value) / divisor, -1.0f, 1.0f);
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

InputManager::~InputManager() {
    shutdown();
}

void InputManager::initialize(const Window& window, bool captureMouse) {
    m_state = {};
    m_quitRequested = false;
    m_resizeRequested = false;
    clearBufferedInput();
    setMouseCaptured(window, captureMouse);
    openFirstGamepad();
}

void InputManager::shutdown() {
    closeGamepad();
}

void InputManager::update(const Window& window, const EventObserver& eventObserver, InputCapture capture,
                          MouseCapturePolicy mouseCapturePolicy) {
    m_quitRequested = false;
    m_resizeRequested = false;
    const InputState previous = m_state;
    m_state = {};
    // A cursor held under Persistent is given back when the policy becomes
    // WhileRightMouseDown (an editor leaving its game view), which only
    // captures for a right-button drag that starts later.
    if (m_mouseCaptured && m_mouseCapturePolicy == MouseCapturePolicy::Persistent &&
        mouseCapturePolicy == MouseCapturePolicy::WhileRightMouseDown) {
        setMouseCaptured(window, false);
    }
    m_mouseCapturePolicy = mouseCapturePolicy;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (eventObserver) {
            eventObserver(event);
        }
        handleEvent(window, event, capture, mouseCapturePolicy);
    }

    updateState(capture, previous);
    m_fixedEdges.accumulate(m_state);
}

const InputState& InputManager::state() const {
    return m_state;
}

InputState InputManager::fixedState() const {
    return m_fixedEdges.apply(m_state);
}

void InputManager::consumeFixedPresses() {
    m_fixedEdges.consume();
}

void InputManager::clearBufferedInput() {
    m_fixedEdges.clear();
}

bool InputManager::quitRequested() const {
    return m_quitRequested;
}

bool InputManager::resizeRequested() const {
    return m_resizeRequested;
}

void InputManager::handleEvent(const Window& window, const SDL_Event& event, InputCapture capture,
                               MouseCapturePolicy mouseCapturePolicy) {
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        m_quitRequested = true;
    }

    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESTORED) {
        m_resizeRequested = true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !capture.keyboard) {
        setMouseCaptured(window, false);
    }

    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !capture.keyboard) {
        if (const std::optional<Key> key = keyForScancode(event.key.scancode)) {
            m_state.setKeyPressed(*key);
            m_activeDevice = InputDevice::KeyboardMouse;
        }
    }

    if (event.type == SDL_EVENT_KEY_UP && !capture.keyboard) {
        if (const std::optional<Key> key = keyForScancode(event.key.scancode)) {
            m_state.setKeyReleased(*key);
            m_activeDevice = InputDevice::KeyboardMouse;
        }
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (!m_mouseCaptured) {
            const bool shouldCapture =
                mouseCapturePolicy == MouseCapturePolicy::Persistent || event.button.button == SDL_BUTTON_RIGHT;
            if (!capture.mouse && shouldCapture) {
                setMouseCaptured(window, true);
                return;
            }
        }
        if (!capture.mouse) {
            const std::optional<MouseButton> button = mouseButtonForSdlButton(event.button.button);
            if (button) {
                m_state.setMouseButtonPressed(*button);
                m_activeDevice = InputDevice::KeyboardMouse;
            }
        }
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && !capture.mouse) {
        if (const std::optional<MouseButton> button = mouseButtonForSdlButton(event.button.button)) {
            m_state.setMouseButtonReleased(*button);
            m_activeDevice = InputDevice::KeyboardMouse;
        }
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && m_mouseCaptured &&
        mouseCapturePolicy == MouseCapturePolicy::WhileRightMouseDown && event.button.button == SDL_BUTTON_RIGHT) {
        setMouseCaptured(window, false);
    }

    if (event.type == SDL_EVENT_MOUSE_WHEEL && !capture.mouse) {
        const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        m_state.mouseWheelDelta += direction * glm::vec2(event.wheel.x, event.wheel.y);
        m_activeDevice = InputDevice::KeyboardMouse;
    }

    if (event.type == SDL_EVENT_GAMEPAD_ADDED && !m_gamepad) {
        m_gamepad = SDL_OpenGamepad(event.gdevice.which);
        if (m_gamepad) {
            m_gamepadId = event.gdevice.which;
        }
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && event.gdevice.which == m_gamepadId) {
        closeGamepad();
        openFirstGamepad();
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        m_activeDevice = InputDevice::Gamepad;
    }
}

void InputManager::updateState(InputCapture capture, const InputState& previous) {
    m_state.mouseCaptured = m_mouseCaptured;
    m_state.gamepadConnected = m_gamepad != nullptr;

    float mouseX = 0.0f;
    float mouseY = 0.0f;
    const SDL_MouseButtonFlags mouseButtons = SDL_GetRelativeMouseState(&mouseX, &mouseY);
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    SDL_GetMouseState(&pointerX, &pointerY);
    m_state.mousePosition = {pointerX, pointerY};

    if (!capture.mouse) {
        m_state.mouseDelta = {mouseX, mouseY};
        for (size_t i = 0; i < InputState::mouseButtonCount; ++i) {
            const MouseButton button = static_cast<MouseButton>(i);
            m_state.setMouseButtonDown(button, (mouseButtons & mouseButtonMask(button)) != 0);
        }
        if (mouseX != 0.0f || mouseY != 0.0f) {
            m_activeDevice = InputDevice::KeyboardMouse;
        }
    }

    if (!capture.keyboard) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        for (size_t i = 0; i < InputState::keyCount; ++i) {
            const Key key = static_cast<Key>(i);
            SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
            switch (key) {
            case Key::W:
                scancode = SDL_SCANCODE_W;
                break;
            case Key::A:
                scancode = SDL_SCANCODE_A;
                break;
            case Key::S:
                scancode = SDL_SCANCODE_S;
                break;
            case Key::D:
                scancode = SDL_SCANCODE_D;
                break;
            case Key::Q:
                scancode = SDL_SCANCODE_Q;
                break;
            case Key::E:
                scancode = SDL_SCANCODE_E;
                break;
            case Key::LeftShift:
                scancode = SDL_SCANCODE_LSHIFT;
                break;
            case Key::RightShift:
                scancode = SDL_SCANCODE_RSHIFT;
                break;
            case Key::LeftControl:
                scancode = SDL_SCANCODE_LCTRL;
                break;
            case Key::RightControl:
                scancode = SDL_SCANCODE_RCTRL;
                break;
            case Key::Space:
                scancode = SDL_SCANCODE_SPACE;
                break;
            case Key::F:
                scancode = SDL_SCANCODE_F;
                break;
            case Key::R:
                scancode = SDL_SCANCODE_R;
                break;
            case Key::Escape:
                scancode = SDL_SCANCODE_ESCAPE;
                break;
            case Key::Count:
                break;
            }
            m_state.setKeyDown(key, keys[scancode]);
        }
    }

    if (m_gamepad) {
        for (size_t i = 0; i < InputState::gamepadButtonCount; ++i) {
            const GamepadButton button = static_cast<GamepadButton>(i);
            const bool down = SDL_GetGamepadButton(m_gamepad, sdlGamepadButton(button));
            m_state.setGamepadButtonDown(button, down);
            m_state.setGamepadButtonPressed(button, down && !previous.gamepadButtonDown(button));
            m_state.setGamepadButtonReleased(button, !down && previous.gamepadButtonDown(button));
            if (down && !previous.gamepadButtonDown(button)) {
                m_activeDevice = InputDevice::Gamepad;
            }
        }
        for (size_t i = 0; i < InputState::gamepadAxisCount; ++i) {
            const GamepadAxis axis = static_cast<GamepadAxis>(i);
            const bool trigger = axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
            const float value = normalizeGamepadAxis(SDL_GetGamepadAxis(m_gamepad, sdlGamepadAxis(axis)), trigger);
            m_state.setGamepadAxis(axis, value);
            if (std::abs(value) > 0.2f && std::abs(value - previous.gamepadAxis(axis)) > 0.01f) {
                m_activeDevice = InputDevice::Gamepad;
            }
        }
    } else if (previous.gamepadConnected) {
        for (size_t i = 0; i < InputState::gamepadButtonCount; ++i) {
            const GamepadButton button = static_cast<GamepadButton>(i);
            m_state.setGamepadButtonReleased(button, previous.gamepadButtonDown(button));
        }
        m_activeDevice = InputDevice::KeyboardMouse;
    }
    m_state.activeDevice = m_activeDevice;
    m_state.applyCapture(capture);
}

void InputManager::openFirstGamepad() {
    if (m_gamepad) {
        return;
    }
    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    if (gamepads && count > 0) {
        m_gamepad = SDL_OpenGamepad(gamepads[0]);
        if (m_gamepad) {
            m_gamepadId = gamepads[0];
        }
    }
    SDL_free(gamepads);
}

void InputManager::closeGamepad() {
    if (m_gamepad) {
        SDL_CloseGamepad(m_gamepad);
        m_gamepad = nullptr;
    }
    m_gamepadId = 0;
}

void InputManager::setMouseCaptured(const Window& window, bool captured) {
    m_mouseCaptured = captured;
    SDL_SetWindowRelativeMouseMode(window.handle(), captured);
}
