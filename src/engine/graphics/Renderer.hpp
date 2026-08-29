#pragma once
#include <volk.h>
#include "vulkan/VulkanContext.hpp"
#include "surface/Surface.hpp"
#include "../window/Window.hpp"
class Renderer{

public:
bool init(const Window& window);
void run();
void shutdown();

private:

    VulkanContext m_context;
    Surface m_surface;
};
