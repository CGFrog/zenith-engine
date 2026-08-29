#pragma once
#include <volk.h>
#include "Window.hpp"
#include <SDL3/SDL_vulkan.h>
#include <vector>

class Surface {
public:
    bool createSurface(VkInstance instance, SDL_Window window);
    VkSurfaceKHR handle() const;
private:
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
};