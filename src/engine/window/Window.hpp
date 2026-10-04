#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

class Window {
  public:
    bool init();
    void shutdown();

    SDL_Window* handle() const;

    static constexpr uint32_t defaultWidth = 1280;
    static constexpr uint32_t defaultHeight = 720;
    uint32_t width{defaultWidth};
    uint32_t height{defaultHeight};
    // A frame capture turns both off: other programs then cannot maximize,
    // snap, or resize the window (SDL pins a fixed-size window's maximized and
    // tracking sizes to its size), nor send it keyboard shortcuts by giving it
    // focus, so every captured frame renders at the requested size.
    bool resizable{true};
    bool focusable{true};

  private:
    SDL_Window* m_window{nullptr};
};
