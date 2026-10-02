#include "LocalShadowPass.hpp"

#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

namespace {
// Bound while local shadows are off or unused, so the descriptor stays valid
// without holding a full atlas.
constexpr uint32_t placeholderAtlasSize = 64;
// Rasterization bias in the depth units of the atlas format; the shading side
// adds a slope-scaled normal offset (mesh.frag).
constexpr float constantDepthBias = 1.0f;
constexpr float slopeDepthBias = 2.0f;

void transitionAtlas(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags2 sourceStages, VkAccessFlags2 sourceAccess,
                     VkPipelineStageFlags2 destinationStages, VkAccessFlags2 destinationAccess) {
    const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = sourceStages,
                                        .srcAccessMask = sourceAccess,
                                        .dstStageMask = destinationStages,
                                        .dstAccessMask = destinationAccess,
                                        .oldLayout = oldLayout,
                                        .newLayout = newLayout,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = image,
                                        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                                             .baseMipLevel = 0,
                                                             .levelCount = 1,
                                                             .baseArrayLayer = 0,
                                                             .layerCount = 1}};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace

bool LocalShadowPass::init(const VulkanContext& context, uint32_t frameCount, VkFormat depthFormat,
                           VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout materialLayout) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_depthFormat = depthFormat;
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_LINEAR,
                                      .minFilter = VK_FILTER_LINEAR,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .compareEnable = VK_TRUE,
                                      .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                                      .maxLod = 0.0f,
                                      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create local shadow sampler")) {
        shutdown();
        return false;
    }
    ShaderModule vertex(m_device);
    ShaderModule fragment(m_device);
    if (!vertex.load(shaderRoot() / "local_shadow.vert.spv") || !fragment.load(shaderRoot() / "shadow.frag.spv") ||
        !m_pipeline.initShadowPipeline(m_device, depthFormat, cameraLayout, materialLayout, vertex.handle(),
                                       fragment.handle(), context.pipelineCache())) {
        shutdown();
        return false;
    }
    m_frames.resize(frameCount);
    for (FrameData& frame : m_frames) {
        const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .size = sizeof(GpuLocalShadowTile) * maximumTiles,
                                        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                                 .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VmaAllocationInfo info{};
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &frame.tiles, &frame.allocation, &info),
                         "Create local shadow tile buffer")) {
            shutdown();
            return false;
        }
        frame.mapped = info.pMappedData;
        std::memset(frame.mapped, 0, sizeof(GpuLocalShadowTile) * maximumTiles);
        vmaFlushAllocation(m_allocator, frame.allocation, 0, VK_WHOLE_SIZE);
    }
    if (!createAtlas(placeholderAtlasSize)) {
        shutdown();
        return false;
    }
    return true;
}

bool LocalShadowPass::createAtlas(uint32_t size) {
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = m_depthFormat,
                                  .extent = {.width = size, .height = size, .depth = 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    Atlas atlas{.size = size};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &atlas.image, &atlas.allocation, nullptr),
                     "Create local shadow atlas")) {
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = atlas.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = m_depthFormat,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &atlas.view), "Create local shadow atlas view")) {
        destroyAtlas(atlas);
        return false;
    }
    m_atlas = atlas;
    m_atlasInitialized = false;
    m_cache.clear();
    return true;
}

void LocalShadowPass::destroyAtlas(const Atlas& atlas) const {
    if (atlas.view) {
        vkDestroyImageView(m_device, atlas.view, nullptr);
    }
    if (atlas.image) {
        vmaDestroyImage(m_allocator, atlas.image, atlas.allocation);
    }
}

void LocalShadowPass::writeDescriptors(uint32_t frameIndex, std::span<const VkDescriptorSet> cameraSets) {
    FrameData& frame = m_frames[frameIndex];
    const VkDescriptorImageInfo atlas{
        .sampler = m_sampler, .imageView = m_atlas.view, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo tiles{.buffer = frame.tiles, .range = VK_WHOLE_SIZE};
    std::vector<VkWriteDescriptorSet> writes;
    for (VkDescriptorSet set : cameraSets) {
        if (set == VK_NULL_HANDLE) {
            continue;
        }
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = set,
                          .dstBinding = atlasBinding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          .pImageInfo = &atlas});
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstSet = set,
                          .dstBinding = tileBinding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          .pBufferInfo = &tiles});
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    frame.boundAtlas = m_atlas.view;
}

uint32_t LocalShadowPass::requiredAtlasSize(LocalShadowQuality quality, bool shadowRequested) const {
    if (quality == LocalShadowQuality::Off) {
        return placeholderAtlasSize;
    }
    if (shadowRequested) {
        return localShadowProfile(quality).atlasSize;
    }
    return m_atlas.size;
}

std::vector<int32_t> LocalShadowPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                             uint64_t completionSerial, const LocalShadowFrameInput& input,
                                             std::span<const VkDescriptorSet> cameraSets, VkDescriptorSet materialSet,
                                             DeferredReleaseQueue& releases, OptionalPassTimestamps& timestamps,
                                             const DrawCasters& drawCasters, RenderStats& stats) {
    const LocalShadowProfile profile = localShadowProfile(input.quality);
    const std::vector<LocalShadowRequest> requests =
        buildLocalShadowRequests(input.lights, input.cameraPosition, profile);
    const uint32_t atlasSize = requiredAtlasSize(input.quality, !requests.empty());
    if (atlasSize != m_atlas.size) {
        const Atlas retired = m_atlas;
        if (createAtlas(atlasSize)) {
            releases.defer(completionSerial, [this, retired] { destroyAtlas(retired); });
        } else {
            // createAtlas left the previous atlas bound; the next frame retries.
            std::cerr << "Local shadows keep the previous atlas size\n";
        }
    }
    if (m_frames[frameIndex].boundAtlas != m_atlas.view) {
        writeDescriptors(frameIndex, cameraSets);
    }

    const LocalShadowAtlas layout = allocateLocalShadowAtlas(
        atlasSize == m_atlas.size ? requests : std::span<const LocalShadowRequest>{}, m_atlas.size, m_atlas.size);
    m_cache.synchronize(layout, input.lights, input.changedCasters, input.invalidateAll);
    const std::vector<uint64_t> scheduled = m_cache.scheduleUpdates(input.cameraPosition, profile.updateBudgetTiles);
    stats.localShadowLightsRejected = layout.rejectedLights;

    if (!m_atlasInitialized || !scheduled.empty()) {
        const bool timed = !scheduled.empty();
        if (timed) {
            timestamps.write(FrameTimestamp::localShadowsBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        }
        renderTiles(commandBuffer, scheduled, cameraSets, materialSet, drawCasters, stats);
        if (timed) {
            timestamps.write(FrameTimestamp::localShadowsEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        }
    }
    uploadTiles(frameIndex);

    uint32_t cached = 0;
    for (const CachedLocalShadowTile& tile : m_cache.tiles()) {
        cached += tile.rendered ? 1u : 0u;
    }
    stats.localShadowTilesCached = cached - std::min(cached, stats.localShadowTilesRendered);
    stats.localShadowTilesPending = m_cache.pendingTileCount();
    stats.localShadowAtlasBytes = atlasBytes();

    std::vector<int32_t> firstTiles(input.lights.size(), -1);
    for (size_t index = 0; index < input.lights.size(); ++index) {
        const std::optional<uint32_t> first = m_cache.shadowTile(input.lights[index].stableId);
        const uint32_t faces = localShadowFaceCount(input.lights[index].type);
        if (first && *first + faces <= maximumTiles) {
            firstTiles[index] = static_cast<int32_t>(*first);
        }
    }
    return firstTiles;
}

void LocalShadowPass::renderTiles(VkCommandBuffer commandBuffer, std::span<const uint64_t> scheduled,
                                  std::span<const VkDescriptorSet> cameraSets, VkDescriptorSet materialSet,
                                  const DrawCasters& drawCasters, RenderStats& stats) {
    const bool clearWholeAtlas = !m_atlasInitialized;
    if (clearWholeAtlas) {
        transitionAtlas(commandBuffer, m_atlas.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    } else {
        transitionAtlas(commandBuffer, m_atlas.image, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    const VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                          .imageView = m_atlas.view,
                                          .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                          .loadOp = clearWholeAtlas ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                    : VK_ATTACHMENT_LOAD_OP_LOAD,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}}};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .renderArea = {.offset = {0, 0}, .extent = {m_atlas.size, m_atlas.size}},
                                    .layerCount = 1,
                                    .pDepthAttachment = &depth};
    vkCmdBeginRendering(commandBuffer, &rendering);
    if (!scheduled.empty()) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.handle());
        vkCmdSetDepthBias(commandBuffer, constantDepthBias, 0.0f, slopeDepthBias);
        const VkDescriptorSet sets[]{cameraSets.front(), materialSet};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.layout(), 0,
                                static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
    }
    const std::span<const CachedLocalShadowTile> tiles = m_cache.tiles();
    for (const uint64_t stableId : scheduled) {
        const std::optional<uint32_t> first = m_cache.firstTile(stableId);
        if (!first) {
            continue;
        }
        const uint32_t faces = localShadowFaceCount(tiles[*first].type);
        if (*first + faces > maximumTiles) {
            continue;
        }
        bool drawn = true;
        for (uint32_t face = 0; face < faces && drawn; ++face) {
            const uint32_t tileIndex = *first + face;
            const LocalShadowTile& placement = tiles[tileIndex].tile;
            const VkRect2D area{.offset = {static_cast<int32_t>(placement.x), static_cast<int32_t>(placement.y)},
                                .extent = {placement.size, placement.size}};
            if (!clearWholeAtlas) {
                const VkClearAttachment clear{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                              .clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}}};
                const VkClearRect rect{.rect = area, .baseArrayLayer = 0, .layerCount = 1};
                vkCmdClearAttachments(commandBuffer, 1, &clear, 1, &rect);
            }
            const VkViewport viewport{.x = static_cast<float>(placement.x),
                                      .y = static_cast<float>(placement.y),
                                      .width = static_cast<float>(placement.size),
                                      .height = static_cast<float>(placement.size),
                                      .minDepth = 0.0f,
                                      .maxDepth = 1.0f};
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &area);
            const glm::vec4 push{static_cast<float>(tileIndex), 0.0f, 0.0f, 0.0f};
            vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            drawn = drawCasters(commandBuffer, tiles[tileIndex].viewProjection);
            ++stats.localShadowTilesRendered;
        }
        if (drawn) {
            m_cache.markRendered(stableId);
        } else {
            // Partially drawn tiles hold neither the old nor the new depth.
            // Later lights may still fit the remaining object capacity.
            m_cache.invalidate(stableId);
        }
    }
    vkCmdEndRendering(commandBuffer);
    transitionAtlas(commandBuffer, m_atlas.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_atlasInitialized = true;
}

void LocalShadowPass::uploadTiles(uint32_t frameIndex) {
    const FrameData& frame = m_frames[frameIndex];
    const std::span<const CachedLocalShadowTile> tiles = m_cache.tiles();
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(tiles.size(), maximumTiles));
    auto* records = static_cast<GpuLocalShadowTile*>(frame.mapped);
    const float atlasScale = 1.0f / static_cast<float>(m_atlas.size);
    for (uint32_t index = 0; index < count; ++index) {
        const CachedLocalShadowTile& tile = tiles[index];
        records[index] = {.viewProjection = tile.renderedViewProjection,
                          .atlasRect = glm::vec4(tile.tile.x, tile.tile.y, tile.tile.size, tile.tile.size) * atlasScale,
                          .params = {2.0f * tile.tanHalfFov / static_cast<float>(tile.tile.size), 0.0f, 0.0f, 0.0f}};
    }
    if (count > 0) {
        vmaFlushAllocation(m_allocator, frame.allocation, 0, sizeof(GpuLocalShadowTile) * count);
    }
}

uint64_t LocalShadowPass::atlasBytes() const {
    // Depth formats used here store four bytes per texel (D32 or D24S8).
    return static_cast<uint64_t>(m_atlas.size) * m_atlas.size * 4u;
}

void LocalShadowPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    m_pipeline.shutdown(m_device);
    destroyAtlas(m_atlas);
    m_atlas = {};
    m_atlasInitialized = false;
    for (FrameData& frame : m_frames) {
        if (frame.tiles) {
            vmaDestroyBuffer(m_allocator, frame.tiles, frame.allocation);
        }
    }
    m_frames.clear();
    m_cache.clear();
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
}
