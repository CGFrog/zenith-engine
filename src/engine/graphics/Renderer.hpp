#pragma once
#include <volk.h>
#include "vulkan/VulkanContext.hpp"
#include "surface/Surface.hpp"
#include "swapchain/Swapchain.hpp"
#include "../window/Window.hpp"
class Renderer{

public:
bool init(const Window& window);
void run();
void shutdown();

private:

    bool createRenderCompletedSemaphores(const VulkanContext& context, const Swapchain& swapchain);
    void destroyRenderCompletedSemaphores();
    VulkanContext m_context;
    Surface m_surface;
    Swapchain swapchain;
    std::vector<VkSemaphore> m_renderCompleteSemaphores;

};
