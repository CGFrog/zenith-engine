#include "UploadContext.hpp"

#include "../vulkan/VulkanCheck.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <cstring>

bool UploadContext::init(const VulkanContext& context) {
    m_device = context.device();
    m_queue = context.gfxQueue();
    m_allocator = context.vmaAllocator();
    VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                 .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                          VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                 .queueFamilyIndex = context.gfxQueueIndex()};
    if (!checkVulkan(vkCreateCommandPool(m_device, &pool, nullptr, &m_pool), "Create reusable upload pool")) {
        return false;
    }
    VkCommandBufferAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = m_pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1};
    if (!checkVulkan(vkAllocateCommandBuffers(m_device, &allocate, &m_command), "Allocate reusable upload command")) {
        shutdown();
        return false;
    }
    VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!checkVulkan(vkCreateFence(m_device, &fence, nullptr, &m_fence), "Create upload fence")) {
        shutdown();
        return false;
    }
    return true;
}

VkBuffer UploadContext::stage(const void* data, VkDeviceSize size) {
    if (!data || size == 0 || !m_allocator) {
        return VK_NULL_HANDLE;
    }
    if (size > m_stagingCapacity) {
        if (m_stagingBuffer) {
            vmaDestroyBuffer(m_allocator, m_stagingBuffer, m_stagingAllocation);
        }
        m_stagingBuffer = VK_NULL_HANDLE;
        m_stagingAllocation = VK_NULL_HANDLE;
        m_stagingMapped = nullptr;
        m_stagingCapacity = 0;
        VkBufferCreateInfo buffer{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                           .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};
        if (!checkVulkan(
                vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_stagingBuffer, &m_stagingAllocation, &info),
                "Create reusable staging buffer")) {
            return VK_NULL_HANDLE;
        }
        m_stagingMapped = info.pMappedData;
        m_stagingCapacity = size;
    }
    std::memcpy(m_stagingMapped, data, static_cast<size_t>(size));
    if (!checkVulkan(vmaFlushAllocation(m_allocator, m_stagingAllocation, 0, size), "Flush staging buffer")) {
        return VK_NULL_HANDLE;
    }
    return m_stagingBuffer;
}

bool UploadContext::submit(const std::function<void(VkCommandBuffer)>& record) {
    if (!m_command || !record) {
        return false;
    }
    if (!checkVulkan(vkResetFences(m_device, 1, &m_fence), "Reset upload fence") ||
        !checkVulkan(vkResetCommandBuffer(m_command, 0), "Reset upload command")) {
        return false;
    }
    VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!checkVulkan(vkBeginCommandBuffer(m_command, &begin), "Begin resource upload")) {
        return false;
    }
    record(m_command);
    if (!checkVulkan(vkEndCommandBuffer(m_command), "End resource upload")) {
        return false;
    }
    VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &m_command};
    return checkVulkan(vkQueueSubmit(m_queue, 1, &submit, m_fence), "Submit resource upload") &&
           checkVulkan(vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX), "Wait for resource upload");
}

void UploadContext::shutdown() {
    if (m_allocator && m_stagingBuffer) {
        vmaDestroyBuffer(m_allocator, m_stagingBuffer, m_stagingAllocation);
    }
    if (m_device && m_fence) {
        vkDestroyFence(m_device, m_fence, nullptr);
    }
    if (m_device && m_pool) {
        vkDestroyCommandPool(m_device, m_pool, nullptr);
    }
    m_device = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_command = VK_NULL_HANDLE;
    m_fence = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_stagingBuffer = VK_NULL_HANDLE;
    m_stagingAllocation = VK_NULL_HANDLE;
    m_stagingMapped = nullptr;
    m_stagingCapacity = 0;
}
