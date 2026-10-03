#include "DepthPyramid.hpp"

#include "../DepthPyramidLayout.hpp"
#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>

namespace {
// Mirrors maximumLevels in shaders/depth_pyramid_single_pass.comp. Covers
// storage up to 65535 texels per axis.
constexpr uint32_t maximumLevels = 16;
constexpr uint32_t perLevelWorkgroupSize = 8;
// Readers of a finished pyramid: occlusion culling and screen-space effects.
constexpr VkPipelineStageFlags2 readerStages =
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

// Mirrors Settings in shaders/depth_pyramid.comp.
struct PerLevelSettings {
    glm::ivec2 outputExtent{0};
    glm::ivec2 sourceExtent{0};
    int32_t sourceLevel{0};
    int32_t copyDepth{0};
};

// Mirrors Settings in shaders/depth_pyramid_single_pass.comp.
struct SinglePassSettings {
    glm::ivec2 baseExtent{0};
    uint32_t levelCount{0};
    uint32_t workgroupCount{0};
    uint32_t conditional{0};
    uint32_t conditionIndex{0};
};

// Bindings of shaders/depth_pyramid_single_pass.comp.
enum SinglePassBinding : uint32_t {
    sourceDepthBinding = 0,
    levelsBinding = 1,
    counterBinding = 2,
    conditionBinding = 3,
};

VkImageSubresourceRange colorLevels(uint32_t baseLevel, uint32_t levelCount) {
    return {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = baseLevel,
            .levelCount = levelCount,
            .baseArrayLayer = 0,
            .layerCount = 1};
}

VkImageMemoryBarrier2 generalImageBarrier(VkImage image, VkImageSubresourceRange levels,
                                          VkPipelineStageFlags2 sourceStages, VkAccessFlags2 sourceAccess,
                                          VkPipelineStageFlags2 destinationStages, VkAccessFlags2 destinationAccess) {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = sourceStages,
            .srcAccessMask = sourceAccess,
            .dstStageMask = destinationStages,
            .dstAccessMask = destinationAccess,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = levels};
}

void imageBarrier(VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2& barrier) {
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

VkDescriptorSetLayoutBinding computeBinding(uint32_t binding, VkDescriptorType type, uint32_t count = 1) {
    return {.binding = binding,
            .descriptorType = type,
            .descriptorCount = count,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}

glm::uvec2 toVector(VkExtent2D extent) {
    return {extent.width, extent.height};
}

VkSamplerCreateInfo pointSampler() {
    return {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = static_cast<float>(maximumLevels)};
}
} // namespace

bool DepthPyramid::init(const VulkanContext& context, VkExtent2D storageExtent, VkImageView depthView) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    // ZENITH_DEPTH_PYRAMID_PER_LEVEL keeps the per-level path for A/B
    // comparisons; it also always rebuilds, like builds before Milestone 25.
    const bool perLevelRequested = SDL_getenv("ZENITH_DEPTH_PYRAMID_PER_LEVEL") != nullptr;
    m_singlePass = context.storageImageArrayDynamicIndexingSupported() && !perLevelRequested;
    if (!m_singlePass && !perLevelRequested) {
        context.log().info("Depth pyramid: shaderStorageImageArrayDynamicIndexing is unsupported; "
                           "building one dispatch per level");
    }
    const VkSamplerCreateInfo sampler = pointSampler();
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create depth-pyramid sampler") ||
        !(m_singlePass ? createSinglePassResources(context) : createPerLevelResources(context)) ||
        !createImage(storageExtent, depthView)) {
        shutdown();
        return false;
    }
    return true;
}

bool DepthPyramid::createSinglePassResources(const VulkanContext& context) {
    const std::array<VkDescriptorSetLayoutBinding, 4> bindings{
        {computeBinding(sourceDepthBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(levelsBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maximumLevels),
         computeBinding(counterBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
         computeBinding(conditionBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)}};
    const VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                                                 .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create single-pass depth-pyramid descriptor layout")) {
        return false;
    }
    const VkBufferCreateInfo counter{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = sizeof(uint32_t),
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateBuffer(m_allocator, &counter, &allocation, &m_counter, &m_counterAllocation, nullptr),
                     "Create depth-pyramid workgroup counter")) {
        return false;
    }
    ShaderModule shader(m_device);
    return shader.load(shaderRoot() / "depth_pyramid_single_pass.comp.spv") &&
           m_pipeline.init(m_device, m_layout, shader.handle(), context.pipelineCache(), sizeof(SinglePassSettings));
}

bool DepthPyramid::createPerLevelResources(const VulkanContext& context) {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{
        {computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
         computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)}};
    const VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    const std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maximumLevels * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maximumLevels},
    }};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                          .maxSets = maximumLevels,
                                          .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                          .pPoolSizes = poolSizes.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create depth-pyramid descriptor layout") ||
        !checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool), "Create depth-pyramid pool")) {
        return false;
    }
    ShaderModule shader(m_device);
    return shader.load(shaderRoot() / "depth_pyramid.comp.spv") &&
           m_pipeline.init(m_device, m_layout, shader.handle(), context.pipelineCache(), sizeof(PerLevelSettings));
}

bool DepthPyramid::createImage(VkExtent2D storageExtent, VkImageView depthView) {
    m_storageExtent = storageExtent;
    m_depthView = depthView;
    m_renderExtent = {};
    m_builtLevels = 0;
    m_layoutInitialized = false;
    const uint32_t levels = std::min(depth_pyramid::levelCount(toVector(storageExtent)), maximumLevels);
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {storageExtent.width, storageExtent.height, 1},
                                  .mipLevels = levels,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &m_image, &m_allocation, nullptr),
                     "Create depth pyramid")) {
        return false;
    }
    VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = m_image,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = format,
                               .subresourceRange = colorLevels(0, levels)};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &m_view), "Create depth-pyramid view")) {
        return false;
    }
    m_levelViews.resize(levels, VK_NULL_HANDLE);
    for (uint32_t level = 0; level < levels; ++level) {
        view.subresourceRange = colorLevels(level, 1);
        if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &m_levelViews[level]),
                         "Create depth-pyramid level view")) {
            return false;
        }
    }
    return m_singlePass || writePerLevelDescriptors(depthView);
}

bool DepthPyramid::writePerLevelDescriptors(VkImageView depthView) {
    const uint32_t levels = storageLevels();
    const std::vector<VkDescriptorSetLayout> layouts(levels, m_layout);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_pool,
                                               .descriptorSetCount = levels,
                                               .pSetLayouts = layouts.data()};
    m_levelDescriptors.resize(levels, VK_NULL_HANDLE);
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, m_levelDescriptors.data()),
                     "Allocate depth-pyramid descriptors")) {
        m_levelDescriptors.clear();
        return false;
    }
    const VkDescriptorImageInfo sourceDepth{
        .sampler = m_sampler, .imageView = depthView, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo sourcePyramid{
        .sampler = m_sampler, .imageView = m_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    for (uint32_t level = 0; level < levels; ++level) {
        const VkDescriptorImageInfo output{.imageView = m_levelViews[level], .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        const auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* info) {
            return VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = m_levelDescriptors[level],
                                        .dstBinding = binding,
                                        .descriptorCount = 1,
                                        .descriptorType = type,
                                        .pImageInfo = info};
        };
        const std::array<VkWriteDescriptorSet, 3> writes{
            {write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sourceDepth),
             write(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sourcePyramid),
             write(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output)}};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return true;
}

bool DepthPyramid::resize(VkExtent2D storageExtent, VkImageView depthView) {
    destroyImage();
    return createImage(storageExtent, depthView);
}

void DepthPyramid::prepareFirstUse(VkCommandBuffer commandBuffer) {
    if (m_counter && !m_counterInitialized) {
        vkCmdFillBuffer(commandBuffer, m_counter, 0, VK_WHOLE_SIZE, 0);
        m_counterInitialized = true;
    }
    if (m_layoutInitialized) {
        return;
    }
    imageBarrier(commandBuffer, {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                 .dstStageMask = readerStages,
                                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = m_image,
                                 .subresourceRange = colorLevels(0, storageLevels())});
    m_layoutInitialized = true;
}

void DepthPyramid::build(VkCommandBuffer commandBuffer, VkExtent2D renderExtent,
                         std::optional<DepthChangeCount> depthChanges) {
    prepareFirstUse(commandBuffer);
    const glm::uvec2 baseExtent = glm::min(toVector(renderExtent), toVector(m_storageExtent));
    const uint32_t levels = std::min(depth_pyramid::levelCount(baseExtent), storageLevels());
    if (m_singlePass) {
        recordSinglePass(commandBuffer, baseExtent, levels, depthChanges);
    } else {
        recordPerLevel(commandBuffer, baseExtent, levels);
    }
    m_renderExtent = {baseExtent.x, baseExtent.y};
    m_builtLevels = levels;
}

void DepthPyramid::recordSinglePass(VkCommandBuffer commandBuffer, glm::uvec2 baseExtent, uint32_t levels,
                                    std::optional<DepthChangeCount> depthChanges) {
    // Earlier readers (last frame's effects, this frame's late cull) finish
    // before any level is overwritten. The memory barrier orders the counter
    // after its initial fill and the previous build, and makes the change
    // count's compute or transfer writes visible. Barriers span every storage
    // level because the dispatch binds them all, even past levelCount.
    const VkImageMemoryBarrier2 levelsBarrier =
        generalImageBarrier(m_image, colorLevels(0, storageLevels()), readerStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkMemoryBarrier2 bufferBarrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .memoryBarrierCount = 1,
                                      .pMemoryBarriers = &bufferBarrier,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &levelsBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    const VkDescriptorImageInfo sourceDepth{
        .sampler = m_sampler, .imageView = m_depthView, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    // Entries past the storage levels repeat the last view; the shader never
    // writes levels at or beyond levelCount.
    std::array<VkDescriptorImageInfo, maximumLevels> levelImages{};
    for (uint32_t level = 0; level < maximumLevels; ++level) {
        levelImages[level] = {.imageView = m_levelViews[std::min(level, storageLevels() - 1)],
                              .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    }
    const VkDescriptorBufferInfo counter{.buffer = m_counter, .offset = 0, .range = VK_WHOLE_SIZE};
    // Unconditional builds bind the counter in place of a change count; the
    // shader then never reads it.
    const VkDescriptorBufferInfo condition{
        .buffer = depthChanges ? depthChanges->buffer : m_counter, .offset = 0, .range = VK_WHOLE_SIZE};
    const std::array<VkWriteDescriptorSet, 4> writes{{
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = sourceDepthBinding,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &sourceDepth},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = levelsBinding,
         .descriptorCount = maximumLevels,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = levelImages.data()},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = counterBinding,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &counter},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = conditionBinding,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &condition},
    }};
    const glm::uvec2 workgroups = depth_pyramid::singlePassWorkgroups(baseExtent);
    const SinglePassSettings settings{.baseExtent = glm::ivec2(baseExtent),
                                      .levelCount = levels,
                                      .workgroupCount = workgroups.x * workgroups.y,
                                      .conditional = depthChanges ? 1u : 0u,
                                      .conditionIndex = depthChanges ? depthChanges->index : 0u};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.handle());
    vkCmdPushDescriptorSet(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.layout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());
    vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatch(commandBuffer, workgroups.x, workgroups.y, 1);
    levelBarrier(commandBuffer, 0, storageLevels());
}

void DepthPyramid::recordPerLevel(VkCommandBuffer commandBuffer, glm::uvec2 baseExtent, uint32_t levels) {
    // Earlier readers (last frame's effects, this frame's late cull) finish
    // before any level is overwritten.
    imageBarrier(commandBuffer,
                 generalImageBarrier(m_image, colorLevels(0, levels), readerStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT));
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.handle());
    for (uint32_t level = 0; level < levels; ++level) {
        const glm::uvec2 output = depth_pyramid::levelExtent(baseExtent, level);
        const glm::uvec2 source = level == 0 ? baseExtent : depth_pyramid::levelExtent(baseExtent, level - 1);
        const PerLevelSettings settings{.outputExtent = glm::ivec2(output),
                                        .sourceExtent = glm::ivec2(source),
                                        .sourceLevel = static_cast<int32_t>(level == 0 ? 0 : level - 1),
                                        .copyDepth = level == 0 ? 1 : 0};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.layout(), 0, 1,
                                &m_levelDescriptors[level], 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                           &settings);
        vkCmdDispatch(commandBuffer, (output.x + perLevelWorkgroupSize - 1) / perLevelWorkgroupSize,
                      (output.y + perLevelWorkgroupSize - 1) / perLevelWorkgroupSize, 1);
        // Each level feeds the next level and then the pyramid's readers.
        levelBarrier(commandBuffer, level, 1);
    }
}

void DepthPyramid::levelBarrier(VkCommandBuffer commandBuffer, uint32_t baseLevel, uint32_t levelCount) const {
    imageBarrier(commandBuffer,
                 generalImageBarrier(m_image, colorLevels(baseLevel, levelCount),
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                     readerStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
}

void DepthPyramid::destroyImage() {
    if (!m_levelDescriptors.empty() && m_pool) {
        vkFreeDescriptorSets(m_device, m_pool, static_cast<uint32_t>(m_levelDescriptors.size()),
                             m_levelDescriptors.data());
    }
    m_levelDescriptors.clear();
    for (VkImageView view : m_levelViews) {
        if (view) {
            vkDestroyImageView(m_device, view, nullptr);
        }
    }
    m_levelViews.clear();
    if (m_view) {
        vkDestroyImageView(m_device, m_view, nullptr);
    }
    if (m_image) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
    }
    m_view = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_depthView = VK_NULL_HANDLE;
    m_layoutInitialized = false;
    m_builtLevels = 0;
    m_renderExtent = {};
}

void DepthPyramid::shutdown() {
    if (!m_device) {
        return;
    }
    destroyImage();
    m_pipeline.shutdown(m_device);
    if (m_counter) {
        vmaDestroyBuffer(m_allocator, m_counter, m_counterAllocation);
    }
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_pool) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    }
    if (m_layout) {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
    m_counter = VK_NULL_HANDLE;
    m_counterAllocation = VK_NULL_HANDLE;
    m_counterInitialized = false;
    m_sampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
}
