#include "MotionBlurPass.hpp"

#include "engine/graphics/shader/ShaderModule.hpp"
#include "engine/graphics/vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>

namespace {
constexpr uint32_t workgroupSize = 8;
constexpr uint32_t sampledInputCount = 2;
constexpr uint32_t storageImageCount = 3;
constexpr uint32_t bindingCount = sampledInputCount + storageImageCount;

// Mirrors MotionBlurSettings in shaders/motion_blur_*.comp.
struct MotionBlurSettings {
    glm::mat4 currentToPreviousClip{1.0f};
    glm::vec4 colorRegion{0.0f};
    glm::vec2 motionRegion{0.0f};
    float velocityScale{0.0f};
    float radiusPixels{0.0f};
    glm::ivec2 tileCount{0};
    int32_t tileSize{0};
    int32_t sampleCount{0};
};

static_assert(sizeof(MotionBlurSettings) == 112, "MotionBlurSettings must match the shader push-constant block");

VkImageMemoryBarrier2 generalBarrier(VkImage image, VkImageLayout oldLayout, VkPipelineStageFlags2 sourceStages,
                                     VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStages,
                                     VkAccessFlags2 destinationAccess) {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = sourceStages,
            .srcAccessMask = sourceAccess,
            .dstStageMask = destinationStages,
            .dstAccessMask = destinationAccess,
            .oldLayout = oldLayout,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1}};
}

void pipelineBarrier(VkCommandBuffer commandBuffer, std::initializer_list<VkImageMemoryBarrier2> barriers) {
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                      .pImageMemoryBarriers = barriers.begin()};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

uint32_t groupCount(uint32_t items) {
    return (items + workgroupSize - 1) / workgroupSize;
}
} // namespace

bool MotionBlurPass::init(const VulkanContext& context, uint32_t frameCount, VkExtent2D outputExtent) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_extent = outputExtent;
    if (!createDescriptors(frameCount)) {
        return false;
    }
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule tileMax(m_device);
    ShaderModule neighborMax(m_device);
    ShaderModule reconstruct(m_device);
    return tileMax.load(shaders / "motion_blur_tile_max.comp.spv") &&
           neighborMax.load(shaders / "motion_blur_neighbor_max.comp.spv") &&
           reconstruct.load(shaders / "motion_blur_reconstruct.comp.spv") &&
           m_tileMax.init(m_device, m_layout, tileMax.handle(), context.pipelineCache(), sizeof(MotionBlurSettings)) &&
           m_neighborMax.init(m_device, m_layout, neighborMax.handle(), context.pipelineCache(),
                              sizeof(MotionBlurSettings)) &&
           m_reconstruct.init(m_device, m_layout, reconstruct.handle(), context.pipelineCache(),
                              sizeof(MotionBlurSettings));
}

bool MotionBlurPass::createDescriptors(uint32_t frameCount) {
    std::array<VkDescriptorSetLayoutBinding, bindingCount> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding] = {.binding = binding,
                             .descriptorType = binding < sampledInputCount ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                           : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                             .descriptorCount = 1,
                             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    const VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create motion blur descriptor layout")) {
        return false;
    }
    const VkDescriptorPoolSize sizes[]{
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = frameCount * sampledInputCount},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = frameCount * storageImageCount}};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = frameCount,
                                          .poolSizeCount = static_cast<uint32_t>(std::size(sizes)),
                                          .pPoolSizes = sizes};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool), "Create motion blur descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_layout);
    m_descriptors.resize(frameCount);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_pool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, m_descriptors.data()),
                     "Allocate motion blur descriptors")) {
        return false;
    }
    // The shaders read every input with texelFetch.
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_NEAREST,
                                      .minFilter = VK_FILTER_NEAREST,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create motion blur sampler");
}

glm::uvec2 MotionBlurPass::tileImageExtent() const {
    // The smallest tiles over the largest colour region.
    return motion_blur::tileCount({m_extent.width, m_extent.height}, motion_blur::tileGranularity);
}

bool MotionBlurPass::createTarget(Target& target, VkFormat format, VkExtent2D extent, const char* name) {
    const VkImageCreateInfo image{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {.width = std::max(extent.width, 1u), .height = std::max(extent.height, 1u), .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &target.image, &target.allocation, nullptr),
                     name)) {
        target = {};
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = target.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = format,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    return checkVulkan(vkCreateImageView(m_device, &view, nullptr, &target.view), name);
}

bool MotionBlurPass::ensureTargets() {
    if (m_blurred.image) {
        return true;
    }
    const glm::uvec2 tiles = tileImageExtent();
    const VkExtent2D tileExtent{tiles.x, tiles.y};
    if (!createTarget(m_blurred, colorFormat, m_extent, "Create motion blur target") ||
        !createTarget(m_tileVelocity, tileFormat, tileExtent, "Create motion blur tile velocities") ||
        !createTarget(m_neighborVelocity, tileFormat, tileExtent, "Create motion blur neighbour velocities")) {
        destroyTargets();
        return false;
    }
    return true;
}

void MotionBlurPass::destroyTargets() {
    for (Target* target : {&m_blurred, &m_tileVelocity, &m_neighborVelocity}) {
        if (target->view) {
            vkDestroyImageView(m_device, target->view, nullptr);
        }
        if (target->image) {
            vmaDestroyImage(m_allocator, target->image, target->allocation);
        }
        *target = {};
    }
}

void MotionBlurPass::resize(VkExtent2D outputExtent) {
    destroyTargets();
    m_extent = outputExtent;
}

void MotionBlurPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    destroyTargets();
    m_tileMax.shutdown(m_device);
    m_neighborMax.shutdown(m_device);
    m_reconstruct.shutdown(m_device);
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
    m_descriptors.clear();
}

void MotionBlurPass::writeDescriptors(VkDescriptorSet descriptor, const MotionBlurInput& input) const {
    const VkDescriptorImageInfo images[bindingCount]{
        {.sampler = m_sampler, .imageView = input.sceneColor, .imageLayout = input.sceneColorLayout},
        {.sampler = m_sampler, .imageView = input.motion, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.imageView = m_tileVelocity.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.imageView = m_neighborVelocity.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.imageView = m_blurred.view, .imageLayout = blurredLayout}};
    std::array<VkWriteDescriptorSet, bindingCount> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                           .dstSet = descriptor,
                           .dstBinding = binding,
                           .descriptorCount = 1,
                           .descriptorType = binding < sampledInputCount ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                         : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           .pImageInfo = &images[binding]};
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool MotionBlurPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const MotionBlurInput& input,
                            const motion_blur::FramePlan& plan) {
    if (!ensureTargets()) {
        return false;
    }
    const VkDescriptorSet descriptor = m_descriptors[frameIndex];
    writeDescriptors(descriptor, input);

    // Every texel read this frame is written first, so earlier contents are
    // discarded. Earlier frames read the tiles in compute and the blurred
    // scene in compute (bloom, exposure) and fragment (post) shaders.
    constexpr VkPipelineStageFlags2 compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags2 readers = compute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    pipelineBarrier(commandBuffer, {generalBarrier(m_tileVelocity.image, VK_IMAGE_LAYOUT_UNDEFINED, compute, 0, compute,
                                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
                                    generalBarrier(m_neighborVelocity.image, VK_IMAGE_LAYOUT_UNDEFINED, compute, 0,
                                                   compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
                                    generalBarrier(m_blurred.image, VK_IMAGE_LAYOUT_UNDEFINED, readers, 0, compute,
                                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)});

    const MotionBlurSettings settings{
        .currentToPreviousClip = input.currentToPreviousClip,
        .colorRegion = {static_cast<float>(input.colorRegion.width), static_cast<float>(input.colorRegion.height),
                        1.0f / static_cast<float>(std::max(input.colorRegion.width, 1u)),
                        1.0f / static_cast<float>(std::max(input.colorRegion.height, 1u))},
        .motionRegion = {static_cast<float>(input.motionRegion.width), static_cast<float>(input.motionRegion.height)},
        .velocityScale = plan.velocityScale,
        .radiusPixels = plan.radiusPixels,
        .tileCount = glm::ivec2(plan.tiles),
        .tileSize = static_cast<int32_t>(plan.tileSize),
        .sampleCount = static_cast<int32_t>(plan.samples)};
    const auto dispatch = [&](const ComputePipeline& pipeline, uint32_t groupsX, uint32_t groupsY) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0, 1, &descriptor, 0,
                                nullptr);
        vkCmdPushConstants(commandBuffer, pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings),
                           &settings);
        vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
    };
    const auto tilesWritten = [&](VkImage image) {
        pipelineBarrier(commandBuffer,
                        {generalBarrier(image, VK_IMAGE_LAYOUT_GENERAL, compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                        compute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT)});
    };
    // One workgroup reduces each tile.
    dispatch(m_tileMax, plan.tiles.x, plan.tiles.y);
    tilesWritten(m_tileVelocity.image);
    dispatch(m_neighborMax, groupCount(plan.tiles.x), groupCount(plan.tiles.y));
    tilesWritten(m_neighborVelocity.image);
    dispatch(m_reconstruct, groupCount(input.colorRegion.width), groupCount(input.colorRegion.height));
    pipelineBarrier(commandBuffer,
                    {generalBarrier(m_blurred.image, blurredLayout, compute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                    readers, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT)});
    return true;
}

uint64_t MotionBlurPass::targetBytes() const {
    if (!m_blurred.image) {
        return 0;
    }
    // Every target is four 16-bit channels per texel.
    constexpr uint64_t texelBytes = sizeof(uint16_t) * 4;
    const glm::uvec2 tiles = tileImageExtent();
    return texelBytes *
           (static_cast<uint64_t>(m_extent.width) * m_extent.height + 2 * static_cast<uint64_t>(tiles.x) * tiles.y);
}
