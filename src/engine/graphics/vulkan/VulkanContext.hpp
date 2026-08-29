#pragma once
#include <volk.h>
#include <vk_mem_alloc.h>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <SDL3/SDL_vulkan.h>
#include <iostream>

class VulkanContext {
public:

bool initInstance();
void closeInstance();

private:
    
constexpr static uint32_t VulkanVersion {VK_API_VERSION_1_4};
VkInstance m_instance;

};