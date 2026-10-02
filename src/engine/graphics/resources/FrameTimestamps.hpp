#pragma once

#include <volk.h>

#include <cstdint>

// Timestamp query slots in each frame's pool. Slots below firstOptionalPass
// are written unconditionally by the passes that own them (see
// FrameResources::collectTimings for the pairs). The pairs from
// firstOptionalPass up bracket passes that a frame may skip.
namespace FrameTimestamp {
inline constexpr uint32_t firstOptionalPass = 21;
inline constexpr uint32_t depthPrepassBegin = 21;
inline constexpr uint32_t depthPrepassEnd = 22;
inline constexpr uint32_t depthPyramidBegin = 23;
inline constexpr uint32_t depthPyramidEnd = 24;
inline constexpr uint32_t lightClustersBegin = 25;
inline constexpr uint32_t lightClustersEnd = 26;
// Environment precompute only records work when its inputs change.
inline constexpr uint32_t environmentPrecomputeBegin = 27;
inline constexpr uint32_t environmentPrecomputeEnd = 28;
inline constexpr uint32_t skyboxBegin = 29;
inline constexpr uint32_t skyboxEnd = 30;
// Local-light shadow tiles; skipped when every tile reused its cached depth.
inline constexpr uint32_t localShadowsBegin = 31;
inline constexpr uint32_t localShadowsEnd = 32;
// GPU culling of the dirty directional shadow cascades (GPU-driven path only).
inline constexpr uint32_t shadowCullBegin = 33;
inline constexpr uint32_t shadowCullEnd = 34;
// Per-pixel reflection probe upload and cluster assignment.
inline constexpr uint32_t reflectionProbeClustersBegin = 35;
inline constexpr uint32_t reflectionProbeClustersEnd = 36;
inline constexpr uint32_t count = 37;
} // namespace FrameTimestamp

// Writes the optional-pass timestamps of one frame's command buffer. Each slot
// is written at most once; closeSkippedPasses writes every slot the frame did
// not reach, back to back, so the whole pool becomes available and skipped
// passes decode as zero. A null pool disables every write.
class OptionalPassTimestamps {
  public:
    OptionalPassTimestamps(VkCommandBuffer commandBuffer, VkQueryPool queries)
        : m_commandBuffer(commandBuffer)
        , m_queries(queries) {}

    void write(uint32_t query, VkPipelineStageFlags2 stage) {
        const uint64_t bit = slotBit(query);
        if (m_queries == VK_NULL_HANDLE || bit == 0 || (m_written & bit) != 0) {
            return;
        }
        vkCmdWriteTimestamp2(m_commandBuffer, stage, m_queries, query);
        m_written |= bit;
    }

    void closeSkippedPasses() {
        for (uint32_t query = FrameTimestamp::firstOptionalPass; query < FrameTimestamp::count; ++query) {
            write(query, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        }
    }

  private:
    static uint64_t slotBit(uint32_t query) {
        if (query < FrameTimestamp::firstOptionalPass || query >= FrameTimestamp::count) {
            return 0;
        }
        return uint64_t{1} << (query - FrameTimestamp::firstOptionalPass);
    }

    VkCommandBuffer m_commandBuffer{VK_NULL_HANDLE};
    VkQueryPool m_queries{VK_NULL_HANDLE};
    uint64_t m_written{0};
};
