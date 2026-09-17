#include "InputManager.hpp"

#include "../window/Window.hpp"

#include <SDL3/SDL.h>

void InputManager::initialize(const Window& window, bool captureMouse) {
    setMouseCaptured(window, captureMouse);
}

void InputManager::update(const Window& window) {
    m_resizeRequested = false;
    m_jumpPressed = false;
    m_state = {};

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        handleEvent(window, event);
    }

    updateState();
}

const InputState& InputManager::state() const {
    return m_state;
}

bool InputManager::quitRequested() const {
    return m_quitRequested;
}

bool InputManager::resizeRequested() const {
    return m_resizeRequested;
}

void InputManager::handleEvent(const Window& window, const SDL_Event& event) {
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        m_quitRequested = true;
    }

    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESTORED) {
        m_resizeRequested = true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        setMouseCaptured(window, false);
    }

    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_SPACE) {
        m_jumpPressed = true;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !m_mouseCaptured) {
        setMouseCaptured(window, true);
    }
}

void InputManager::updateState() {
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    SDL_GetRelativeMouseState(&mouseX, &mouseY);

    if (!m_mouseCaptured) {
        return;
    }

    const bool* keys = SDL_GetKeyboardState(nullptr);
    m_state.movement.x = static_cast<float>(keys[SDL_SCANCODE_D]) - static_cast<float>(keys[SDL_SCANCODE_A]);
    m_state.movement.y = static_cast<float>(keys[SDL_SCANCODE_E]) - static_cast<float>(keys[SDL_SCANCODE_Q]);
    m_state.movement.z = static_cast<float>(keys[SDL_SCANCODE_W]) - static_cast<float>(keys[SDL_SCANCODE_S]);
    m_state.look = {mouseX, mouseY};
    m_state.sprint = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    m_state.jumpPressed = m_jumpPressed;
}

void InputManager::setMouseCaptured(const Window& window, bool captured) {
    m_mouseCaptured = captured;
    SDL_SetWindowRelativeMouseMode(window.handle(), captured);
}
