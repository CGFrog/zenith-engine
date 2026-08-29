#pragma once
#include <volk.h>
#include <vk_mem_alloc.h>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <SDL3/SDL_vulkan.h>
#include <iostream>
#include "../surface/Surface.hpp"

class VulkanContext {
public:

bool initInstance();
bool initPhysicalDevice(const Surface& surface);
bool initGfxQueue(const Surface& surface);
bool initDevice();
bool initVMA();

void closeInstance();


VkInstance instance() const;
VkPhysicalDevice pDevice() const;

private:
    
constexpr static uint32_t VulkanVersion {VK_API_VERSION_1_4};
VkInstance m_instance{VK_NULL_HANDLE};
VkPhysicalDevice m_pDevice{VK_NULL_HANDLE};
VkDevice m_device{VK_NULL_HANDLE};
uint32_t m_gfxQueueFamIdx {UINT32_MAX};
VkQueue m_gfxQueue{VK_NULL_HANDLE};
VmaAllocator vmaAllocator {VK_NULL_HANDLE};
};