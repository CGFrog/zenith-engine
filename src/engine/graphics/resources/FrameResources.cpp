#include "FrameResources.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "FrameTimestamps.hpp"
#include "engine/diagnostics/Timestamp.hpp"
#include <algorithm>
#include <limits>

namespace {
constexpr uint64_t frameFenceTimeout = std::numeric_limits<uint64_t>::max();
}

namespace {
// Pairs are listed in collectTimings; 17 -> 18 brackets exposure, 19 -> 20 the
// temporal resolve, and FrameTimestamp names the optional passes after them.
constexpr uint32_t timestampQueryCount = FrameTimestamp::count;
} // namespace

bool FrameResources::init(VkDevice device, uint32_t queueFamily, float timestampPeriod, uint32_t timestampValidBits) {
    m_timestampPeriod = timestampPeriod;
    m_timestampValidBits = timestampValidBits;
    if (!createCommandResources(device, queueFamily)) {
        return false;
    }
    return createSynchronization(device) && createTimestampQueries(device);
}

bool FrameResources::createTimestampQueries(VkDevice device) {
    if (m_timestampValidBits == 0) {
        return true;
    }
    VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                               .queryType = VK_QUERY_TYPE_TIMESTAMP,
                               .queryCount = timestampQueryCount};
    return checkVulkan(vkCreateQueryPool(device, &info, nullptr, &m_timestampQueries),
                       "Create frame timestamp query pool");
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
    if (!checkVulkan(vkBeginCommandBuffer(m_commandBuffer, &begin), "Begin command buffer")) {
        return false;
    }
    if (m_timestampQueries) {
        vkCmdResetQueryPool(m_commandBuffer, m_timestampQueries, 0, timestampQueryCount);
        writeTimestamp(0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    }
    return true;
}

bool FrameResources::endRecording() const {
    return checkVulkan(vkEndCommandBuffer(m_commandBuffer), "End command buffer");
}

bool FrameResources::submit(VkDevice device, VkQueue queue, VkSemaphore renderComplete, VkSemaphore uploadTimeline,
                            uint64_t uploadValue, VkSemaphore secondUploadTimeline, uint64_t secondUploadValue) const {
    VkSemaphoreSubmitInfo waits[3]{{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                    .semaphore = m_imageAcquired,
                                    .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT}};
    uint32_t waitCount = 1;
    const auto appendTimelineWait = [&](VkSemaphore semaphore, uint64_t value, VkPipelineStageFlags2 stages) {
        if (semaphore && value > 0) {
            waits[waitCount++] = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = semaphore,
                                  .value = value,
                                  .stageMask = stages};
        }
    };
    appendTimelineWait(uploadTimeline, uploadValue,
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                           VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    appendTimelineWait(secondUploadTimeline, secondUploadValue, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

    VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = renderComplete;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = m_commandBuffer;
    VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = waitCount;
    submit.pWaitSemaphoreInfos = waits;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;

    if (!checkVulkan(vkResetFences(device, 1, &m_fence), "Reset frame fence")) {
        return false;
    }
    return checkVulkan(vkQueueSubmit2(queue, 1, &submit, m_fence), "Submit frame");
}

void FrameResources::writeTimestamp(uint32_t query, VkPipelineStageFlags2 stage) const {
    if (m_timestampQueries) {
        vkCmdWriteTimestamp2(m_commandBuffer, stage, m_timestampQueries, query);
    }
}

bool FrameResources::collectTimings(VkDevice device, GpuFrameTimings& timings) {
    timings = {};
    if (!m_timestampsPending) {
        return true;
    }
    uint64_t values[timestampQueryCount]{};
    const VkResult result = vkGetQueryPoolResults(device, m_timestampQueries, 0, timestampQueryCount, sizeof(values),
                                                  values, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return checkVulkan(result, "Read frame timestamp queries");
    }
    const auto elapsed = [&](uint32_t first, uint32_t last) {
        return timestampMilliseconds(values[first], values[last], m_timestampValidBits, m_timestampPeriod);
    };
    m_timestampsPending = false;
    timings.valid = true;
    timings.frame = m_sampleFrame;
    timings.frameMilliseconds = elapsed(0, 5);
    timings.shadowMilliseconds = elapsed(0, 1);
    timings.opaqueMilliseconds = elapsed(1, 2);
    // Screen-space reflections (12-13) run inside the 2-3 span, between opaque
    // and blended surfaces.
    timings.blendedMilliseconds = std::max(elapsed(2, 3) - elapsed(12, 13), 0.0);
    timings.sceneMilliseconds = elapsed(1, 3);
    timings.bloomMilliseconds = m_bloomEnabled ? elapsed(6, 7) : 0.0;
    timings.postMilliseconds = elapsed(14, 15);
    timings.uiMilliseconds = elapsed(4, 5);
    timings.cullingMilliseconds = elapsed(8, 9);
    timings.temporalMilliseconds = m_temporalEnabled ? elapsed(19, 20) : 0.0;
    timings.ambientOcclusionMilliseconds = m_ambientOcclusionEnabled ? elapsed(10, 11) : 0.0;
    timings.reflectionsMilliseconds = m_reflectionsEnabled ? elapsed(12, 13) : 0.0;
    timings.uploadWaitMilliseconds = m_uploadWaitEnabled ? elapsed(0, 16) : 0.0;
    timings.exposureMilliseconds = elapsed(17, 18);
    timings.depthPrepassMilliseconds = elapsed(FrameTimestamp::depthPrepassBegin, FrameTimestamp::depthPrepassEnd);
    timings.depthPyramidMilliseconds = elapsed(FrameTimestamp::depthPyramidBegin, FrameTimestamp::depthPyramidEnd);
    timings.lightClusterMilliseconds = elapsed(FrameTimestamp::lightClustersBegin, FrameTimestamp::lightClustersEnd);
    timings.environmentPrecomputeMilliseconds =
        elapsed(FrameTimestamp::environmentPrecomputeBegin, FrameTimestamp::environmentPrecomputeEnd);
    timings.skyboxMilliseconds = elapsed(FrameTimestamp::skyboxBegin, FrameTimestamp::skyboxEnd);
    timings.shadowCullingMilliseconds = elapsed(FrameTimestamp::shadowCullBegin, FrameTimestamp::shadowCullEnd);
    timings.localShadowMilliseconds = elapsed(FrameTimestamp::localShadowsBegin, FrameTimestamp::localShadowsEnd);
    timings.reflectionProbeClusterMilliseconds =
        elapsed(FrameTimestamp::reflectionProbeClustersBegin, FrameTimestamp::reflectionProbeClustersEnd);
    timings.volumetricFogMilliseconds = elapsed(FrameTimestamp::volumetricFogBegin, FrameTimestamp::volumetricFogEnd);
    timings.irradianceVolumeMilliseconds =
        elapsed(FrameTimestamp::irradianceVolumeBegin, FrameTimestamp::irradianceVolumeEnd);
    timings.fsrEasuMilliseconds = elapsed(FrameTimestamp::fsrEasuBegin, FrameTimestamp::fsrEasuEnd);
    timings.fsrRcasMilliseconds = elapsed(FrameTimestamp::fsrRcasBegin, FrameTimestamp::fsrRcasEnd);
    return true;
}

void FrameResources::shutdown(VkDevice device) {
    if (m_timestampQueries) {
        vkDestroyQueryPool(device, m_timestampQueries, nullptr);
    }
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
    m_timestampQueries = VK_NULL_HANDLE;
    m_timestampsPending = false;
    m_bloomEnabled = false;
    m_temporalEnabled = false;
    m_ambientOcclusionEnabled = false;
    m_reflectionsEnabled = false;
    m_uploadWaitEnabled = false;
    m_timestampValidBits = 0;
    m_sampleFrame = 0;
}
