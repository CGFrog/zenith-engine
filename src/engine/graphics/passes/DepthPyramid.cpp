#include "DepthPyramid.hpp"

#include "../DepthPyramidLayout.hpp"
#include "../shader/ShaderModule.hpp"
#include "../vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>

namespace {
constexpr uint32_t maximumLevels = 32;
constexpr uint32_t workgroupSize = 8;
// Readers of a finished pyramid: occlusion culling and screen-space effects.
constexpr VkPipelineStageFlags2 readerStages =
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

// Mirrors Settings in shaders/depth_pyramid.comp.
struct BuildSettings {
    glm::ivec2 outputExtent{0};
    glm::ivec2 sourceExtent{0};
    int32_t sourceLevel{0};
    int32_t copyDepth{0};
};

VkImageSubresourceRange colorLevels(uint32_t baseLevel, uint32_t levelCount) {
    return {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = baseLevel,
            .levelCount = levelCount,
            .baseArrayLayer = 0,
            .layerCount = 1};
}

void imageBarrier(VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2& barrier) {
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

VkDescriptorSetLayoutBinding computeBinding(uint32_t binding, VkDescriptorType type) {
    return {
        .binding = binding, .descriptorType = type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
}

glm::uvec2 toVector(VkExtent2D extent) {
    return {extent.width, extent.height};
}
} // namespace

bool DepthPyramid::init(const VulkanContext& context, VkExtent2D storageExtent, VkImageView depthView) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
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
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_NEAREST,
                                      .minFilter = VK_FILTER_NEAREST,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = static_cast<float>(maximumLevels)};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create depth-pyramid descriptor layout") ||
        !checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool), "Create depth-pyramid pool") ||
        !checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create depth-pyramid sampler")) {
        shutdown();
        return false;
    }
    ShaderModule shader(m_device);
    if (!shader.load(shaderRoot() / "depth_pyramid.comp.spv") ||
        !m_pipeline.init(m_device, m_layout, shader.handle(), context.pipelineCache(), sizeof(BuildSettings)) ||
        !createImage(storageExtent, depthView)) {
        shutdown();
        return false;
    }
    return true;
}

bool DepthPyramid::createImage(VkExtent2D storageExtent, VkImageView depthView) {
    m_storageExtent = storageExtent;
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

void DepthPyramid::build(VkCommandBuffer commandBuffer, VkExtent2D renderExtent) {
    prepareFirstUse(commandBuffer);
    const glm::uvec2 baseExtent = glm::min(toVector(renderExtent), toVector(m_storageExtent));
    const uint32_t levels = std::min(depth_pyramid::levelCount(baseExtent), storageLevels());
    // Earlier readers (last frame's effects, this frame's early cull) finish
    // before any level is overwritten.
    imageBarrier(commandBuffer, {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .srcStageMask = readerStages,
                                 .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = m_image,
                                 .subresourceRange = colorLevels(0, levels)});
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.handle());
    for (uint32_t level = 0; level < levels; ++level) {
        const glm::uvec2 output = depth_pyramid::levelExtent(baseExtent, level);
        const glm::uvec2 source = level == 0 ? baseExtent : depth_pyramid::levelExtent(baseExtent, level - 1);
        const BuildSettings settings{.outputExtent = glm::ivec2(output),
                                     .sourceExtent = glm::ivec2(source),
                                     .sourceLevel = static_cast<int32_t>(level == 0 ? 0 : level - 1),
                                     .copyDepth = level == 0 ? 1 : 0};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.layout(), 0, 1,
                                &m_levelDescriptors[level], 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                           &settings);
        vkCmdDispatch(commandBuffer, (output.x + workgroupSize - 1) / workgroupSize,
                      (output.y + workgroupSize - 1) / workgroupSize, 1);
        levelBarrier(commandBuffer, level);
    }
    m_renderExtent = {baseExtent.x, baseExtent.y};
    m_builtLevels = levels;
}

void DepthPyramid::levelBarrier(VkCommandBuffer commandBuffer, uint32_t level) const {
    // Each level feeds the next level and then the pyramid's readers.
    imageBarrier(commandBuffer, {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                 .dstStageMask = readerStages,
                                 .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = m_image,
                                 .subresourceRange = colorLevels(level, 1)});
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
    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_pool) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    }
    if (m_layout) {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
    m_sampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
}
