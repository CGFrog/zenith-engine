#include "FrameResources.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include <limits>

namespace {
constexpr uint64_t frameFenceTimeout = std::numeric_limits<uint64_t>::max();
}

bool FrameResources::init(VkDevice device, uint32_t queueFamily) {
    if (!createCommandResources(device, queueFamily)) {
        return false;
    }
    return createSynchronization(device);
}

bool FrameResources::createCommandResources(VkDevice device, uint32_t queueFamily) {
    VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queueFamily;
    if (!checkVulkan(vkCreateCommandPool(device, &pool, nullptr, &m_commandPool), "Create command pool")) {
        return false;
    }

    VkCommandBufferAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = m_commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    return checkVulkan(vkAllocateCommandBuffers(device, &allocate, &m_commandBuffer), "Allocate command buffer");
}

bool FrameResources::createSynchronization(VkDevice device) {
    VkSemaphoreCreateInfo semaphore{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!checkVulkan(vkCreateSemaphore(device, &semaphore, nullptr, &m_imageAcquired), "Create acquire semaphore")) {
        return false;
    }

    VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return checkVulkan(vkCreateFence(device, &fence, nullptr, &m_fence), "Create frame fence");
}

bool FrameResources::waitUntilAvailable(VkDevice device) const {
    return checkVulkan(vkWaitForFences(device, 1, &m_fence, VK_TRUE, frameFenceTimeout), "Wait for frame");
}

bool FrameResources::beginRecording() const {
    if (!checkVulkan(vkResetCommandBuffer(m_commandBuffer, 0), "Reset command buffer")) {
        return false;
    }
    VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return checkVulkan(vkBeginCommandBuffer(m_commandBuffer, &begin), "Begin command buffer");
}

bool FrameResources::endRecording() const {
    return checkVulkan(vkEndCommandBuffer(m_commandBuffer), "End command buffer");
}

bool FrameResources::submit(VkDevice device, VkQueue queue, VkSemaphore renderComplete) const {
    VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = m_imageAcquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = renderComplete;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = m_commandBuffer;
    VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;

    if (!checkVulkan(vkResetFences(device, 1, &m_fence), "Reset frame fence")) {
        return false;
    }
    return checkVulkan(vkQueueSubmit2(queue, 1, &submit, m_fence), "Submit frame");
}

void FrameResources::shutdown(VkDevice device) {
    if (m_fence) {
        vkDestroyFence(device, m_fence, nullptr);
    }
    if (m_imageAcquired) {
        vkDestroySemaphore(device, m_imageAcquired, nullptr);
    }
    if (m_commandPool) {
        // Destroying the pool also frees its command buffers.
        vkDestroyCommandPool(device, m_commandPool, nullptr);
    }
    m_fence = VK_NULL_HANDLE;
    m_imageAcquired = VK_NULL_HANDLE;
    m_commandPool = VK_NULL_HANDLE;
    m_commandBuffer = VK_NULL_HANDLE;
}
