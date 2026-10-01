#include "UploadContext.hpp"

#include "engine/diagnostics/Timestamp.hpp"

#include "../vulkan/VulkanCheck.hpp"
#include "../vulkan/VulkanContext.hpp"

#include <cstring>
#include <vector>

bool UploadContext::init(const VulkanContext& context, bool preferDedicatedTransferQueue) {
    m_device = context.device();
    const bool useDedicated = preferDedicatedTransferQueue && context.hasDedicatedTransferQueue();
    m_queue = useDedicated ? context.transferQueue() : context.gfxQueue();
    m_queueFamily = useDedicated ? context.transferQueueIndex() : context.gfxQueueIndex();
    m_graphicsQueueFamily = context.gfxQueueIndex();
    m_sharingQueueFamilies = {m_queueFamily, m_graphicsQueueFamily};
    m_sharingQueueFamilyCount = usesDedicatedQueue() ? 2u : 1u;
    m_allocator = context.vmaAllocator();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.pDevice(), &properties);
    m_timestampPeriod = properties.limits.timestampPeriod;
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context.pDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(context.pDevice(), &familyCount, families.data());
    m_timestampValidBits = families[m_queueFamily].timestampValidBits;

    VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                 .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                          VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                 .queueFamilyIndex = m_queueFamily};
    if (!checkVulkan(vkCreateCommandPool(m_device, &pool, nullptr, &m_pool), "Create upload-ring pool")) {
        return false;
    }
    std::array<VkCommandBuffer, ringSize> commands{};
    VkCommandBufferAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = m_pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = ringSize};
    if (!checkVulkan(vkAllocateCommandBuffers(m_device, &allocate, commands.data()), "Allocate upload-ring commands")) {
        shutdown();
        return false;
    }
    for (uint32_t index = 0; index < ringSize; ++index) {
        m_slots[index].command = commands[index];
        VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!checkVulkan(vkCreateFence(m_device, &fence, nullptr, &m_slots[index].fence), "Create upload-ring fence")) {
            shutdown();
            return false;
        }
    }
    if (context.timelineSemaphoreSupported()) {
        VkSemaphoreTypeCreateInfo timelineType{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                               .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                               .initialValue = 0};
        VkSemaphoreCreateInfo timeline{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timelineType};
        if (!checkVulkan(vkCreateSemaphore(m_device, &timeline, nullptr, &m_timelineSemaphore),
                         "Create upload timeline semaphore")) {
            shutdown();
            return false;
        }
    }
    VkQueryPoolCreateInfo queries{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                  .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                  .queryCount = ringSize * 2};
    if (m_timestampValidBits != 0 && !checkVulkan(vkCreateQueryPool(m_device, &queries, nullptr, &m_timestampQueries),
                                                  "Create upload-ring timestamp queries")) {
        shutdown();
        return false;
    }
    return true;
}

VkBuffer UploadContext::stage(const void* data, VkDeviceSize size) {
    if (!data || size == 0 || size > maximumSlotBytes || !m_allocator || m_activeSlot != UINT32_MAX) {
        return VK_NULL_HANDLE;
    }
    if (!retireCompleted(false)) {
        return VK_NULL_HANDLE;
    }
    uint32_t selected = UINT32_MAX;
    for (uint32_t offset = 0; offset < ringSize; ++offset) {
        const uint32_t candidate = (m_nextSlot + offset) % ringSize;
        if (m_slots[candidate].pendingValue == 0) {
            selected = candidate;
            break;
        }
    }
    if (selected == UINT32_MAX) {
        ++m_backpressureEvents;
        return VK_NULL_HANDLE;
    }
    Slot& slot = m_slots[selected];
    if (size > slot.stagingCapacity) {
        const VkCommandBuffer command = slot.command;
        const VkFence fence = slot.fence;
        if (slot.stagingBuffer) {
            vmaDestroyBuffer(m_allocator, slot.stagingBuffer, slot.stagingAllocation);
        }
        slot = {.command = command, .fence = fence};
        VkBufferCreateInfo buffer{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                           .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};
        if (!checkVulkan(
                vmaCreateBuffer(m_allocator, &buffer, &allocation, &slot.stagingBuffer, &slot.stagingAllocation, &info),
                "Create upload-ring staging buffer")) {
            return VK_NULL_HANDLE;
        }
        slot.stagingMapped = info.pMappedData;
        slot.stagingCapacity = size;
    }
    std::memcpy(slot.stagingMapped, data, static_cast<size_t>(size));
    if (!checkVulkan(vmaFlushAllocation(m_allocator, slot.stagingAllocation, 0, size), "Flush staging buffer")) {
        return VK_NULL_HANDLE;
    }
    m_activeSlot = selected;
    m_nextSlot = (selected + 1) % ringSize;
    m_stagedBytes += size;
    return slot.stagingBuffer;
}

bool UploadContext::submit(const std::function<void(VkCommandBuffer)>& record) {
    if (m_activeSlot == UINT32_MAX || !record) {
        return false;
    }
    const uint32_t slotIndex = m_activeSlot;
    Slot& slot = m_slots[slotIndex];
    m_activeSlot = UINT32_MAX;
    if (!checkVulkan(vkResetFences(m_device, 1, &slot.fence), "Reset upload-ring fence") ||
        !checkVulkan(vkResetCommandBuffer(slot.command, 0), "Reset upload-ring command")) {
        return false;
    }
    VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!checkVulkan(vkBeginCommandBuffer(slot.command, &begin), "Begin resource upload")) {
        return false;
    }
    if (m_timestampQueries) {
        vkCmdResetQueryPool(slot.command, m_timestampQueries, slotIndex * 2, 2);
        vkCmdWriteTimestamp2(slot.command,
                             usesDedicatedQueue() ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
                                                  : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             m_timestampQueries, slotIndex * 2);
    }
    record(slot.command);
    if (m_timestampQueries) {
        vkCmdWriteTimestamp2(slot.command,
                             usesDedicatedQueue() ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
                                                  : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             m_timestampQueries, slotIndex * 2 + 1);
    }
    if (!checkVulkan(vkEndCommandBuffer(slot.command), "End resource upload")) {
        return false;
    }
    if (m_timelineSemaphore) {
        const uint64_t previousSubmittedValue = m_lastSubmittedValue;
        slot.pendingValue = m_nextTimelineValue++;
        m_lastSubmittedValue = slot.pendingValue;
        VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                          .commandBuffer = slot.command};
        VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                     .semaphore = m_timelineSemaphore,
                                     .value = slot.pendingValue,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                             .commandBufferInfoCount = 1,
                             .pCommandBufferInfos = &command,
                             .signalSemaphoreInfoCount = 1,
                             .pSignalSemaphoreInfos = &signal};
        if (!checkVulkan(vkQueueSubmit2(m_queue, 1, &submit, VK_NULL_HANDLE), "Submit asynchronous resource upload")) {
            slot.pendingValue = 0;
            m_lastSubmittedValue = previousSubmittedValue;
            return false;
        }
        return true;
    }
    VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &slot.command};
    if (!checkVulkan(vkQueueSubmit(m_queue, 1, &submit, slot.fence), "Submit resource upload")) {
        return false;
    }
    slot.pendingValue = 1;
    return retireCompleted(true);
}

bool UploadContext::retireCompleted(bool waitForAll) {
    uint64_t completedValue = 0;
    if (m_timelineSemaphore) {
        if (waitForAll && m_lastSubmittedValue > 0) {
            VkSemaphoreWaitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                     .semaphoreCount = 1,
                                     .pSemaphores = &m_timelineSemaphore,
                                     .pValues = &m_lastSubmittedValue};
            if (!checkVulkan(vkWaitSemaphores(m_device, &wait, UINT64_MAX), "Retire upload ring")) {
                return false;
            }
        }
        if (!checkVulkan(vkGetSemaphoreCounterValue(m_device, m_timelineSemaphore, &completedValue),
                         "Query upload timeline")) {
            return false;
        }
    }
    m_lastGpuMilliseconds = 0.0;
    for (uint32_t slotIndex = 0; slotIndex < ringSize; ++slotIndex) {
        Slot& slot = m_slots[slotIndex];
        if (slot.pendingValue == 0) {
            continue;
        }
        bool complete = m_timelineSemaphore && slot.pendingValue <= completedValue;
        if (!m_timelineSemaphore) {
            const VkResult fenceResult =
                vkWaitForFences(m_device, 1, &slot.fence, VK_TRUE, waitForAll ? UINT64_MAX : 0);
            complete = fenceResult == VK_SUCCESS;
            if (fenceResult != VK_SUCCESS && fenceResult != VK_TIMEOUT) {
                return checkVulkan(fenceResult, "Poll upload-ring fence");
            }
        }
        if (!complete) {
            continue;
        }
        if (m_timestampQueries) {
            uint64_t timestamps[2]{};
            if (!checkVulkan(vkGetQueryPoolResults(m_device, m_timestampQueries, slotIndex * 2, 2, sizeof(timestamps),
                                                   timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT),
                             "Read upload-ring timestamps")) {
                return false;
            }
            m_lastGpuMilliseconds +=
                timestampMilliseconds(timestamps[0], timestamps[1], m_timestampValidBits, m_timestampPeriod);
        }
        slot.pendingValue = 0;
    }
    return true;
}

void UploadContext::shutdown() {
    retireCompleted(true);
    for (Slot& slot : m_slots) {
        if (m_allocator && slot.stagingBuffer) {
            vmaDestroyBuffer(m_allocator, slot.stagingBuffer, slot.stagingAllocation);
        }
        if (m_device && slot.fence) {
            vkDestroyFence(m_device, slot.fence, nullptr);
        }
    }
    if (m_device && m_timelineSemaphore) {
        vkDestroySemaphore(m_device, m_timelineSemaphore, nullptr);
    }
    if (m_device && m_timestampQueries) {
        vkDestroyQueryPool(m_device, m_timestampQueries, nullptr);
    }
    if (m_device && m_pool) {
        vkDestroyCommandPool(m_device, m_pool, nullptr);
    }
    m_device = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_timestampQueries = VK_NULL_HANDLE;
    m_timelineSemaphore = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
    m_slots = {};
    m_timestampPeriod = 0.0f;
    m_timestampValidBits = 0;
    m_stagedBytes = 0;
    m_lastGpuMilliseconds = 0.0;
    m_nextTimelineValue = 1;
    m_lastSubmittedValue = 0;
    m_nextSlot = 0;
    m_activeSlot = UINT32_MAX;
    m_backpressureEvents = 0;
    m_queueFamily = UINT32_MAX;
    m_graphicsQueueFamily = UINT32_MAX;
    m_sharingQueueFamilies = {};
    m_sharingQueueFamilyCount = 1;
}
