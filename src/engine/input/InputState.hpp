#pragma once

// The input a frame or fixed step sees: keys, mouse, gamepad and dropped files, with no
// platform types, so gameplay code can read it without SDL (InputManager fills it).

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <vector>

struct InputCapture {
    bool keyboard{false};
    bool mouse{false};
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

// A file or folder dropped onto the window from the operating system (for
// example from Explorer). position is where it was dropped, in window
// coordinates (the same space as InputState::mousePosition).
struct DroppedFile {
    std::filesystem::path path;
    glm::vec2 position{0.0f};
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
    // Files and folders dropped onto the window this frame, in drop order.
    // Only the frame's state carries them; fixed-step states never do.
    std::vector<DroppedFile> droppedFiles;

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
