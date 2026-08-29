#include "Renderer.hpp"


bool Renderer::init(const Window& window){
    if (volkInitialize() != VK_SUCCESS) {
        return false;
    }
    if (!m_context.initInstance()){
        return false;
    }
    if (!m_surface.init(m_context.instance(), window.handle())){
        return false;
    }
    if (!m_context.initPhysicalDevice(m_surface)){
        return false;
    }
    if (!m_context.initDevice()){
        return false;
    }
    if (!m_context.initGfxQueue(m_surface)){
        return false;
    }
    if (!m_context.initVMA()){
        return false;
    }
    if (!swapchain.initSwapchain(window, m_surface,m_context)){
        return false;
    }
}

bool Renderer::createRenderCompletedSemaphores(const VulkanContext& context, const Swapchain& swapchain){
	m_renderCompleteSemaphores.resize(swapchain.swapchainImages().size());
	for (VkSemaphore &semaphore : m_renderCompleteSemaphores){
		VkSemaphoreCreateInfo semaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if (vkCreateSemaphore(context.device(), &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
			return false;
		}
	}
    return true;
}

void Renderer::destroyRenderCompletedSemaphores(){
    for (VkSemaphore &semaphore : m_renderCompleteSemaphores){
		vkDestroySemaphore(m_context.device(), semaphore, nullptr);
	}
	m_renderCompleteSemaphores.clear();
}