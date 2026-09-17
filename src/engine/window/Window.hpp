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

  private:
    SDL_Window* m_window{nullptr};
};
