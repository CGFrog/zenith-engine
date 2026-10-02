#include "DepthOfFieldPass.hpp"

#include "engine/graphics/shader/ShaderModule.hpp"
#include "engine/graphics/vulkan/VulkanCheck.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <algorithm>

namespace {
constexpr uint32_t workgroupSize = 8;

// Descriptor bindings shared by the three shaders; each declares only the ones
// it uses.
enum Binding : uint32_t {
    sceneColorBinding = 0,
    motionDepthBinding = 1,
    preparedSampledBinding = 2,
    gatheredSampledBinding = 3,
    preparedStorageBinding = 4,
    gatheredStorageBinding = 5,
    outputStorageBinding = 6,
    bindingCount = 7,
};

bool storageBinding(uint32_t binding) {
    return binding >= preparedStorageBinding;
}

// Mirrors DepthOfFieldSettings in shaders/dof_*.comp.
struct DepthOfFieldSettingsBlock {
    glm::vec4 fullSize{0.0f};
    glm::vec4 gatherSize{0.0f};
    glm::vec2 gatherUvScale{1.0f};
    glm::vec2 depthSize{0.0f};
    float cocScale{0.0f};
    float focusDistance{1.0f};
    float maximumRadius{0.0f};
    float depthScale{0.0f};
    float depthBias{0.0f};
    float ringSpacing{0.0f};
};

glm::vec4 sizeAndReciprocal(glm::uvec2 extent) {
    const glm::vec2 size = glm::max(glm::vec2(extent), glm::vec2(1.0f));
    return {size, 1.0f / size};
}

VkImageMemoryBarrier2 targetBarrier(VkImage image, VkImageLayout oldLayout, VkPipelineStageFlags2 sourceStages,
                                    VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStages,
                                    VkAccessFlags2 destinationAccess) {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = sourceStages,
            .srcAccessMask = sourceAccess,
            .dstStageMask = destinationStages,
            .dstAccessMask = destinationAccess,
            .oldLayout = oldLayout,
            .newLayout = DepthOfFieldPass::outputLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1}};
}

void barrier(VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2& imageBarrier) {
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &imageBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

// Makes a just-written target readable by the next compute dispatch.
void writeToComputeRead(VkCommandBuffer commandBuffer, VkImage image) {
    barrier(commandBuffer, targetBarrier(image, DepthOfFieldPass::outputLayout, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
}

void dispatch(VkCommandBuffer commandBuffer, const ComputePipeline& pipeline, VkDescriptorSet descriptor,
              const DepthOfFieldSettingsBlock& settings, glm::uvec2 extent) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0, 1, &descriptor, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatch(commandBuffer, (extent.x + workgroupSize - 1) / workgroupSize,
                  (extent.y + workgroupSize - 1) / workgroupSize, 1);
}
} // namespace

bool DepthOfFieldPass::init(const VulkanContext& context, uint32_t frameCount, VkExtent2D outputExtent) {
    m_device = context.device();
    m_allocator = context.vmaAllocator();
    m_extent = outputExtent;
    if (!createDescriptors(frameCount)) {
        return false;
    }
    const std::filesystem::path shaders = shaderRoot();
    ShaderModule prepare(m_device);
    ShaderModule gather(m_device);
    ShaderModule composite(m_device);
    return prepare.load(shaders / "dof_prepare.comp.spv") && gather.load(shaders / "dof_gather.comp.spv") &&
           composite.load(shaders / "dof_composite.comp.spv") &&
           m_prepare.init(m_device, m_layout, prepare.handle(), context.pipelineCache(),
                          sizeof(DepthOfFieldSettingsBlock)) &&
           m_gather.init(m_device, m_layout, gather.handle(), context.pipelineCache(),
                         sizeof(DepthOfFieldSettingsBlock)) &&
           m_composite.init(m_device, m_layout, composite.handle(), context.pipelineCache(),
                            sizeof(DepthOfFieldSettingsBlock));
}

bool DepthOfFieldPass::createDescriptors(uint32_t frameCount) {
    std::array<VkDescriptorSetLayoutBinding, bindingCount> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding] = {.binding = binding,
                             .descriptorType = storageBinding(binding) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                       : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             .descriptorCount = 1,
                             .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    const VkDescriptorSetLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = static_cast<uint32_t>(bindings.size()),
                                                 .pBindings = bindings.data()};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_layout),
                     "Create depth of field descriptor layout")) {
        return false;
    }
    constexpr uint32_t storageCount = bindingCount - preparedStorageBinding;
    const VkDescriptorPoolSize sizes[]{
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = frameCount * preparedStorageBinding},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = frameCount * storageCount}};
    const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = frameCount,
                                          .poolSizeCount = static_cast<uint32_t>(std::size(sizes)),
                                          .pPoolSizes = sizes};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool),
                     "Create depth of field descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_layout);
    m_descriptors.resize(frameCount);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = m_pool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, m_descriptors.data()),
                     "Allocate depth of field descriptors")) {
        return false;
    }
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_LINEAR,
                                      .minFilter = VK_FILTER_LINEAR,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.0f};
    return checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create depth of field sampler");
}

bool DepthOfFieldPass::createTarget(Target& target, VkExtent2D extent) {
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = imageFormat,
                                  .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(vmaCreateImage(m_allocator, &image, &allocation, &target.image, &target.allocation, nullptr),
                     "Create depth of field target")) {
        target = {};
        return false;
    }
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = target.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = imageFormat,
                                     .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                          .baseMipLevel = 0,
                                                          .levelCount = 1,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1}};
    return checkVulkan(vkCreateImageView(m_device, &view, nullptr, &target.view), "Create depth of field view");
}

bool DepthOfFieldPass::ensureTargets() {
    if (m_output.image) {
        return true;
    }
    const glm::uvec2 gather = depth_of_field::gatherExtent({m_extent.width, m_extent.height});
    const VkExtent2D gatherExtent{gather.x, gather.y};
    if (!createTarget(m_prepared, gatherExtent) || !createTarget(m_gathered, gatherExtent) ||
        !createTarget(m_output, m_extent)) {
        destroyTargets();
        return false;
    }
    // Fresh targets have no earlier accesses; the first record discards them.
    m_targetsUsed = false;
    return true;
}

void DepthOfFieldPass::destroyTargets() {
    for (Target* target : {&m_prepared, &m_gathered, &m_output}) {
        if (target->view) {
            vkDestroyImageView(m_device, target->view, nullptr);
        }
        if (target->image) {
            vmaDestroyImage(m_allocator, target->image, target->allocation);
        }
        *target = {};
    }
    m_targetsUsed = false;
}

void DepthOfFieldPass::resize(VkExtent2D outputExtent) {
    destroyTargets();
    m_extent = outputExtent;
}

void DepthOfFieldPass::shutdown() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    destroyTargets();
    m_prepare.shutdown(m_device);
    m_gather.shutdown(m_device);
    m_composite.shutdown(m_device);
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

void DepthOfFieldPass::writeDescriptors(VkDescriptorSet descriptor, const DepthOfFieldImages& images) const {
    const std::array<VkDescriptorImageInfo, bindingCount> infos{{
        {.sampler = m_sampler, .imageView = images.color, .imageLayout = images.colorLayout},
        {.sampler = m_sampler,
         .imageView = images.motionDepth,
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.sampler = m_sampler, .imageView = m_prepared.view, .imageLayout = outputLayout},
        {.sampler = m_sampler, .imageView = m_gathered.view, .imageLayout = outputLayout},
        {.imageView = m_prepared.view, .imageLayout = outputLayout},
        {.imageView = m_gathered.view, .imageLayout = outputLayout},
        {.imageView = m_output.view, .imageLayout = outputLayout},
    }};
    std::array<VkWriteDescriptorSet, bindingCount> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                           .dstSet = descriptor,
                           .dstBinding = binding,
                           .descriptorCount = 1,
                           .descriptorType = storageBinding(binding) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           .pImageInfo = &infos[binding]};
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool DepthOfFieldPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex, const DepthOfFieldImages& images,
                              const DepthOfFieldSettings& settings, const depth_of_field::CameraTerms& camera) {
    if (!ensureTargets()) {
        return false;
    }
    const VkDescriptorSet descriptor = m_descriptors[frameIndex];
    writeDescriptors(descriptor, images);

    const glm::uvec2 fullExtent{std::min(images.colorExtent.width, m_extent.width),
                                std::min(images.colorExtent.height, m_extent.height)};
    const glm::uvec2 gatherExtent = depth_of_field::gatherExtent(fullExtent);
    const glm::uvec2 gatherImageExtent = depth_of_field::gatherExtent({m_extent.width, m_extent.height});
    const depth_of_field::CircleOfConfusion circle =
        depth_of_field::circleOfConfusion(settings, camera.verticalFovRadians, static_cast<float>(fullExtent.y));
    const DepthOfFieldSettingsBlock block{
        .fullSize = sizeAndReciprocal(fullExtent),
        .gatherSize = sizeAndReciprocal(gatherExtent),
        .gatherUvScale = glm::vec2(gatherExtent) / glm::vec2(gatherImageExtent),
        .depthSize = glm::max(glm::vec2(images.depthExtent.width, images.depthExtent.height), glm::vec2(1.0f)),
        .cocScale = circle.scale,
        .focusDistance = circle.focusDistance,
        .maximumRadius = circle.maximumRadius,
        .depthScale = camera.depthScale,
        .depthBias = camera.depthBias,
        .ringSpacing = depth_of_field::gatherRingSpacing(settings.quality)};

    // Earlier frames read the gather targets in compute and the output in
    // later compute (bloom, exposure) and fragment (post) shaders. Every texel
    // the passes below read is rewritten first, so the contents are discarded.
    const VkPipelineStageFlags2 earlierReaders =
        m_targetsUsed ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                      : VK_PIPELINE_STAGE_2_NONE;
    const VkImageMemoryBarrier2 discard[]{
        targetBarrier(m_prepared.image, VK_IMAGE_LAYOUT_UNDEFINED, earlierReaders, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
        targetBarrier(m_gathered.image, VK_IMAGE_LAYOUT_UNDEFINED, earlierReaders, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
        targetBarrier(m_output.image, VK_IMAGE_LAYOUT_UNDEFINED, earlierReaders, 0,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)};
    const VkDependencyInfo before{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                  .imageMemoryBarrierCount = static_cast<uint32_t>(std::size(discard)),
                                  .pImageMemoryBarriers = discard};
    vkCmdPipelineBarrier2(commandBuffer, &before);

    dispatch(commandBuffer, m_prepare, descriptor, block, gatherExtent);
    writeToComputeRead(commandBuffer, m_prepared.image);
    dispatch(commandBuffer, m_gather, descriptor, block, gatherExtent);
    writeToComputeRead(commandBuffer, m_gathered.image);
    dispatch(commandBuffer, m_composite, descriptor, block, fullExtent);
    barrier(commandBuffer,
            targetBarrier(m_output.image, outputLayout, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
    m_targetsUsed = true;
    return true;
}

uint64_t DepthOfFieldPass::allocatedBytes() const {
    if (!m_output.image) {
        return 0;
    }
    constexpr uint64_t bytesPerTexel = sizeof(uint16_t) * 4;
    const glm::uvec2 gather = depth_of_field::gatherExtent({m_extent.width, m_extent.height});
    return bytesPerTexel *
           (static_cast<uint64_t>(m_extent.width) * m_extent.height + 2 * static_cast<uint64_t>(gather.x) * gather.y);
}
