#pragma once

// GPU timestamp query slots per pass and frame.

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
// Volumetric fog injection, reprojection, and integration.
inline constexpr uint32_t volumetricFogBegin = 37;
inline constexpr uint32_t volumetricFogEnd = 38;
// Irradiance volume: the probe upload, which records work only when the bound
// volume changes. Probe sampling is part of opaque shading.
inline constexpr uint32_t irradianceVolumeBegin = 39;
inline constexpr uint32_t irradianceVolumeEnd = 40;
// Depth of field (circle of confusion, half-resolution gather, composite).
inline constexpr uint32_t depthOfFieldBegin = 41;
inline constexpr uint32_t depthOfFieldEnd = 42;
// Motion blur velocity tiles and reconstruction, after depth of field.
inline constexpr uint32_t motionBlurBegin = 43;
inline constexpr uint32_t motionBlurEnd = 44;
// Display-referred LUT grade, vignette and film grain after tone mapping.
inline constexpr uint32_t colorGradeBegin = 45;
inline constexpr uint32_t colorGradeEnd = 46;
// Raw post-process pairs: PostProcessPass writes every slot each frame (back
// to back when the pass is skipped), so closeSkippedPasses leaves them alone.
// FSR 1 upsampling (EASU) and sharpening (RCAS); see SpatialUpscalePass.
inline constexpr uint32_t fsrEasuBegin = 47;
inline constexpr uint32_t fsrEasuEnd = 48;
inline constexpr uint32_t fsrRcasBegin = 49;
inline constexpr uint32_t fsrRcasEnd = 50;
inline constexpr uint32_t firstPostProcessPass = fsrEasuBegin;
inline constexpr uint32_t postProcessPassesEnd = fsrRcasEnd + 1;
// Decal cluster assignment; skipped when the frame has no decal.
inline constexpr uint32_t decalClustersBegin = 51;
inline constexpr uint32_t decalClustersEnd = 52;
// GPU particles: emission, simulation, lighting, and sorting, then the
// billboard draw.
inline constexpr uint32_t particleSimulationBegin = 53;
inline constexpr uint32_t particleSimulationEnd = 54;
inline constexpr uint32_t particleDrawBegin = 55;
inline constexpr uint32_t particleDrawEnd = 56;
// Copy of the opaque scene for transmissive surfaces; inside the 2-3 span,
// only on frames that draw one.
inline constexpr uint32_t transmissionBackgroundBegin = 57;
inline constexpr uint32_t transmissionBackgroundEnd = 58;
inline constexpr uint32_t count = 59;
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

    // Leaves the post-process pairs to PostProcessPass, which records later.
    void closeSkippedPasses() {
        for (uint32_t query = FrameTimestamp::firstOptionalPass; query < FrameTimestamp::count; ++query) {
            if (query >= FrameTimestamp::firstPostProcessPass && query < FrameTimestamp::postProcessPassesEnd) {
                continue;
            }
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
