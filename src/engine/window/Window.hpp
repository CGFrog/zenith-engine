#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

class Window {
public:

bool init();
void shutdown();

SDL_Window* handle() const;

uint32_t width{1280};
uint32_t height{720};

private:
SDL_Window* m_window{nullptr};
};