#pragma once

// Mip chain generation by successive blits.

#include <volk.h>

#include <cstdint>
#include <vector>

// Number of levels in a full mip chain down to 1x1 for a width x height image.
uint32_t fullMipChainLength(uint32_t width, uint32_t height);

// One downsampling step of a mip chain: level `sourceMip` (sourceWidth x
// sourceHeight texels) is filtered into level `sourceMip + 1`.
struct MipBlitStep {
    uint32_t sourceMip{0};
    int32_t sourceWidth{0};
    int32_t sourceHeight{0};
    int32_t destinationWidth{0};
    int32_t destinationHeight{0};
};

// The blits that fill levels 1..mipCount-1 from level 0, in recording order.
// mipCount is clamped to the full chain length; zero or one level needs none.
std::vector<MipBlitStep> planMipChainBlits(uint32_t width, uint32_t height, uint32_t mipCount);

struct MipChainBlitTarget {
    VkImage image{VK_NULL_HANDLE};
    // Extent of level 0.
    uint32_t width{0};
    uint32_t height{0};
    // Array layers [0, layerCount) are filtered together (six for a cube).
    uint32_t layerCount{1};
    // At most fullMipChainLength(width, height).
    uint32_t mipCount{1};
};

// Who reads the finished chain. The last barrier on every level makes the
// transfer results available to these stages and accesses; a dedicated
// transfer queue whose consumers wait on a semaphore passes
// ALL_TRANSFER / no access.
struct MipChainConsumers {
    VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT};
    VkAccessFlags2 access{VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
};

// Fills levels 1..mipCount-1 by successive linear blits.
// Layout contract: on entry every level of the layer range is
// TRANSFER_DST_OPTIMAL and level 0 holds transfer-written texels. On exit every
// level is SHADER_READ_ONLY_OPTIMAL and visible to `consumers`. The image needs
// TRANSFER_SRC and TRANSFER_DST usage and a format that supports linear blits.
void recordMipChainBlits(VkCommandBuffer commandBuffer, const MipChainBlitTarget& target, MipChainConsumers consumers);
