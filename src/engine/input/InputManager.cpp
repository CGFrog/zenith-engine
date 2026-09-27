#include "InputManager.hpp"

#include "../window/Window.hpp"

#include <SDL3/SDL.h>
#include <optional>

namespace {
size_t keyIndex(Key key) {
    return static_cast<size_t>(key);
}

size_t mouseButtonIndex(MouseButton button) {
    return static_cast<size_t>(button);
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
} // namespace

bool InputState::keyDown(Key key) const {
    return m_down.test(keyIndex(key));
}

bool InputState::keyPressed(Key key) const {
    return m_pressed.test(keyIndex(key));
}

void InputState::setKeyDown(Key key, bool down) {
    m_down.set(keyIndex(key), down);
}

void InputState::setKeyPressed(Key key, bool pressed) {
    m_pressed.set(keyIndex(key), pressed);
}

bool InputState::mouseButtonDown(MouseButton button) const {
    return m_mouseDown.test(mouseButtonIndex(button));
}

bool InputState::mouseButtonPressed(MouseButton button) const {
    return m_mousePressed.test(mouseButtonIndex(button));
}

void InputState::setMouseButtonDown(MouseButton button, bool down) {
    m_mouseDown.set(mouseButtonIndex(button), down);
}

void InputState::setMouseButtonPressed(MouseButton button, bool pressed) {
    m_mousePressed.set(mouseButtonIndex(button), pressed);
}

void InputManager::initialize(const Window& window, bool captureMouse) {
    m_state = {};
    m_quitRequested = false;
    m_resizeRequested = false;
    clearBufferedInput();
    setMouseCaptured(window, captureMouse);
}

void InputManager::update(const Window& window, const EventObserver& eventObserver, InputCapture capture,
                          MouseCapturePolicy mouseCapturePolicy) {
    m_quitRequested = false;
    m_resizeRequested = false;
    m_state = {};

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (eventObserver) {
            eventObserver(event);
        }
        handleEvent(window, event, capture, mouseCapturePolicy);
    }

    updateState(capture);
    m_pendingPresses |= m_state.m_pressed;
    m_pendingMousePresses |= m_state.m_mousePressed;
    m_pendingMouseWheelDelta += m_state.mouseWheelDelta;
}

const InputState& InputManager::state() const {
    return m_state;
}

InputState InputManager::fixedState() const {
    InputState fixed = m_state;
    fixed.m_pressed = m_pendingPresses;
    fixed.m_mousePressed = m_pendingMousePresses;
    fixed.mouseWheelDelta = m_pendingMouseWheelDelta;
    return fixed;
}

void InputManager::consumeFixedPresses() {
    m_pendingPresses.reset();
    m_pendingMousePresses.reset();
    m_pendingMouseWheelDelta = {};
}

void InputManager::clearBufferedInput() {
    m_pendingPresses.reset();
    m_pendingMousePresses.reset();
    m_pendingMouseWheelDelta = {};
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
            }
        }
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && m_mouseCaptured &&
        mouseCapturePolicy == MouseCapturePolicy::WhileRightMouseDown && event.button.button == SDL_BUTTON_RIGHT) {
        setMouseCaptured(window, false);
    }

    if (event.type == SDL_EVENT_MOUSE_WHEEL && !capture.mouse) {
        const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        m_state.mouseWheelDelta += direction * glm::vec2(event.wheel.x, event.wheel.y);
    }
}

void InputManager::updateState(InputCapture capture) {
    m_state.mouseCaptured = m_mouseCaptured;

    float mouseX = 0.0f;
    float mouseY = 0.0f;
    const SDL_MouseButtonFlags mouseButtons = SDL_GetRelativeMouseState(&mouseX, &mouseY);

    if (capture.mouse) {
        return;
    }

    m_state.mouseDelta = {mouseX, mouseY};
    for (size_t i = 0; i < InputState::mouseButtonCount; ++i) {
        const MouseButton button = static_cast<MouseButton>(i);
        m_state.setMouseButtonDown(button, (mouseButtons & mouseButtonMask(button)) != 0);
    }

    if (capture.keyboard) {
        return;
    }

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
        case Key::Count:
            break;
        }
        m_state.setKeyDown(key, keys[scancode]);
    }
}

void InputManager::setMouseCaptured(const Window& window, bool captured) {
    m_mouseCaptured = captured;
    SDL_SetWindowRelativeMouseMode(window.handle(), captured);
}
