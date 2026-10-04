#include "Window.hpp"

bool Window::init() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return false;
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (!focusable) {
        flags |= SDL_WINDOW_NOT_FOCUSABLE;
    }
    m_window = SDL_CreateWindow("Zenith", width, height, flags);

    if (!m_window) {
        return false;
    }

    return true;
}

SDL_Window* Window::handle() const {
    return m_window;
}

void Window::shutdown() {
    SDL_DestroyWindow(m_window);
    m_window = nullptr;
    SDL_Quit();
}
