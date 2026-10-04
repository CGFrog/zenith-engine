#pragma once

#include <SDL3/SDL_events.h>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>

struct SDL_Gamepad;

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
    Escape,
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

enum class GamepadButton : uint8_t {
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Count,
};

enum class GamepadAxis : uint8_t {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count,
};

enum class InputDevice : uint8_t {
    KeyboardMouse,
    Gamepad,
};

struct InputState {
    bool keyDown(Key key) const;
    bool keyPressed(Key key) const;
    bool keyReleased(Key key) const;
    void setKeyDown(Key key, bool down = true);
    void setKeyPressed(Key key, bool pressed = true);
    void setKeyReleased(Key key, bool released = true);

    bool mouseButtonDown(MouseButton button) const;
    bool mouseButtonPressed(MouseButton button) const;
    bool mouseButtonReleased(MouseButton button) const;
    void setMouseButtonDown(MouseButton button, bool down = true);
    void setMouseButtonPressed(MouseButton button, bool pressed = true);
    void setMouseButtonReleased(MouseButton button, bool released = true);

    bool gamepadButtonDown(GamepadButton button) const;
    bool gamepadButtonPressed(GamepadButton button) const;
    bool gamepadButtonReleased(GamepadButton button) const;
    void setGamepadButtonDown(GamepadButton button, bool down = true);
    void setGamepadButtonPressed(GamepadButton button, bool pressed = true);
    void setGamepadButtonReleased(GamepadButton button, bool released = true);
    float gamepadAxis(GamepadAxis axis) const;
    void setGamepadAxis(GamepadAxis axis, float value);
    void setGamepadConnected(bool connected);
    void setActiveDevice(InputDevice device);
    void applyCapture(InputCapture capture);

    glm::vec2 mouseDelta{0.0f};
    glm::vec2 mouseWheelDelta{0.0f};
    glm::vec2 mousePosition{0.0f};
    bool mouseCaptured{false};
    bool gamepadConnected{false};
    InputDevice activeDevice{InputDevice::KeyboardMouse};

  private:
    friend class InputManager;
    friend class InputEdgeBuffer;
    static constexpr size_t keyCount = static_cast<size_t>(Key::Count);
    static constexpr size_t mouseButtonCount = static_cast<size_t>(MouseButton::Count);
    static constexpr size_t gamepadButtonCount = static_cast<size_t>(GamepadButton::Count);
    static constexpr size_t gamepadAxisCount = static_cast<size_t>(GamepadAxis::Count);
    std::bitset<keyCount> m_down;
    std::bitset<keyCount> m_pressed;
    std::bitset<keyCount> m_released;
    std::bitset<mouseButtonCount> m_mouseDown;
    std::bitset<mouseButtonCount> m_mousePressed;
    std::bitset<mouseButtonCount> m_mouseReleased;
    std::bitset<gamepadButtonCount> m_gamepadDown;
    std::bitset<gamepadButtonCount> m_gamepadPressed;
    std::bitset<gamepadButtonCount> m_gamepadReleased;
    std::array<float, gamepadAxisCount> m_gamepadAxes{};
};

class InputEdgeBuffer {
  public:
    void accumulate(const InputState& state);
    InputState apply(const InputState& state) const;
    void consume();
    void clear();

  private:
    std::bitset<InputState::keyCount> m_keyPresses;
    std::bitset<InputState::keyCount> m_keyReleases;
    std::bitset<InputState::mouseButtonCount> m_mousePresses;
    std::bitset<InputState::mouseButtonCount> m_mouseReleases;
    std::bitset<InputState::gamepadButtonCount> m_gamepadPresses;
    std::bitset<InputState::gamepadButtonCount> m_gamepadReleases;
    glm::vec2 m_mouseWheelDelta{0.0f};
};

class InputManager {
  public:
    ~InputManager();
    void initialize(const Window& window, bool captureMouse);
    void shutdown();
    using EventObserver = std::function<void(const SDL_Event&)>;

    // Persistent captures the mouse on any click (outside UI capture) until
    // Escape; WhileRightMouseDown only while the right button is held. A
    // change from Persistent to WhileRightMouseDown releases a held capture.

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
    void updateState(InputCapture capture, const InputState& previous);
    void openFirstGamepad();
    void closeGamepad();
    void setMouseCaptured(const Window& window, bool captured);

    InputState m_state;
    InputEdgeBuffer m_fixedEdges;
    SDL_Gamepad* m_gamepad{nullptr};
    uint32_t m_gamepadId{0};
    InputDevice m_activeDevice{InputDevice::KeyboardMouse};
    bool m_mouseCaptured{false};
    // The policy of the previous update; see update.
    MouseCapturePolicy m_mouseCapturePolicy{MouseCapturePolicy::Persistent};
    bool m_quitRequested{false};
    bool m_resizeRequested{false};
};
