#pragma once

#include <volk.h>

#include <cstdint>

// Depth-prepass outputs for screen-space effects, valid from the end of the
// prepass until the frame's next record(). PostProcessPass::normalFormat,
// PostProcessPass::specularFormat, and DepthPyramid document the encodings.
struct SceneGeometryViews {
    // SHADER_READ_ONLY_OPTIMAL; covers renderExtent at the top-left.
    VkImageView normals{VK_NULL_HANDLE};
    // SHADER_READ_ONLY_OPTIMAL; same extents as normals. VK_NULL_HANDLE unless
    // the prepass wrote specular reflectance, which it does only on frames
    // that trace screen-space reflections.
    VkImageView specular{VK_NULL_HANDLE};
    // GENERAL; complete opaque depth of the frame.
    VkImageView depthPyramid{VK_NULL_HANDLE};
    VkSampler depthPyramidSampler{VK_NULL_HANDLE};
    uint32_t depthPyramidLevels{0};
    VkExtent2D renderExtent{};
    // Size of the normal target and of pyramid level 0's storage.
    VkExtent2D storageExtent{};

    bool valid() const {
        return normals != VK_NULL_HANDLE;
    }
};
