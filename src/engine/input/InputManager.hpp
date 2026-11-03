#pragma once

// SDL events to InputState: keys, mouse, gamepads, text, dropped files, mouse capture.

#include "InputState.hpp"

#include <SDL3/SDL_events.h>
#include <cstdint>
#include <functional>

struct SDL_Gamepad;

class Window;

enum class MouseCapturePolicy : uint8_t {
    Persistent,
    WhileRightMouseDown,
    // The pointer is free, for a game's menu: a held capture is released and
    // no click captures. Returning to Persistent captures the mouse again.
    Released,
};

class InputManager {
  public:
    ~InputManager();
    void initialize(const Window& window, bool captureMouse);
    void shutdown();
    using EventObserver = std::function<void(const SDL_Event&)>;

    // Persistent captures the mouse on any click (outside UI capture) until
    // Escape; WhileRightMouseDown only while the right button is held;
    // Released never captures. A change from Persistent to
    // WhileRightMouseDown, or to Released, releases a held capture, and a
    // change from Released to Persistent captures the mouse again.

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
