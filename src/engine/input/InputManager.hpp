#pragma once

#include <SDL3/SDL_events.h>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>

class Window;

struct InputCapture {
    bool keyboard{false};
    bool mouse{false};
};

enum class MouseCapturePolicy : uint8_t {
    Persistent,
    WhileRightMouseDown,
};

enum class Key : uint8_t {
    W,
    A,
    S,
    D,
    Q,
    E,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    Space,
    F,
    R,
    Count,
};

enum class MouseButton : uint8_t {
    Left,
    Middle,
    Right,
    X1,
    X2,
    Count,
};

struct InputState {
    bool keyDown(Key key) const;
    bool keyPressed(Key key) const;
    void setKeyDown(Key key, bool down = true);
    void setKeyPressed(Key key, bool pressed = true);

    bool mouseButtonDown(MouseButton button) const;
    bool mouseButtonPressed(MouseButton button) const;
    void setMouseButtonDown(MouseButton button, bool down = true);
    void setMouseButtonPressed(MouseButton button, bool pressed = true);

    glm::vec2 mouseDelta{0.0f};
    glm::vec2 mouseWheelDelta{0.0f};
    bool mouseCaptured{false};

  private:
    friend class InputManager;
    static constexpr size_t keyCount = static_cast<size_t>(Key::Count);
    static constexpr size_t mouseButtonCount = static_cast<size_t>(MouseButton::Count);
    std::bitset<keyCount> m_down;
    std::bitset<keyCount> m_pressed;
    std::bitset<mouseButtonCount> m_mouseDown;
    std::bitset<mouseButtonCount> m_mousePressed;
};

class InputManager {
  public:
    void initialize(const Window& window, bool captureMouse);
    using EventObserver = std::function<void(const SDL_Event&)>;

    void update(const Window& window, const EventObserver& eventObserver = {}, InputCapture capture = {},
                MouseCapturePolicy mouseCapturePolicy = MouseCapturePolicy::Persistent);

    const InputState& state() const;
    InputState fixedState() const;
    void consumeFixedPresses();
    void clearBufferedInput();
    bool quitRequested() const;
    bool resizeRequested() const;

  private:
    void handleEvent(const Window& window, const SDL_Event& event, InputCapture capture,
                     MouseCapturePolicy mouseCapturePolicy);
    void updateState(InputCapture capture);
    void setMouseCaptured(const Window& window, bool captured);

    InputState m_state;
    std::bitset<InputState::keyCount> m_pendingPresses;
    std::bitset<InputState::mouseButtonCount> m_pendingMousePresses;
    glm::vec2 m_pendingMouseWheelDelta{0.0f};
    bool m_mouseCaptured{false};
    bool m_quitRequested{false};
    bool m_resizeRequested{false};
};
