#pragma once

#include <SDL3/SDL_events.h>
#include <glm/glm.hpp>

class Window;

struct InputState {
    glm::vec3 movement{0.0f};
    glm::vec2 look{0.0f};
    bool sprint{false};
    bool jumpPressed{false};
};

class InputManager {
  public:
    void initialize(const Window& window, bool captureMouse);
    void update(const Window& window);

    const InputState& state() const;
    bool quitRequested() const;
    bool resizeRequested() const;

  private:
    void handleEvent(const Window& window, const SDL_Event& event);
    void updateState();
    void setMouseCaptured(const Window& window, bool captured);

    InputState m_state;
    bool m_mouseCaptured{false};
    bool m_quitRequested{false};
    bool m_resizeRequested{false};
    bool m_jumpPressed{false};
};
