// MeshPass: per-view state: camera sets, view targets, creation, resize, destruction.

#include "../Lighting.hpp"
#include "../ReflectionProbes.hpp"
#include "../resources/DescriptorPoolSizes.hpp"
#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <string>

using namespace mesh_pass_detail;

bool MeshPass::initViewPasses(const VulkanContext& context, ViewState& view, VkExtent2D extent, VkImageView depthView) {
    const uint32_t frameCount = static_cast<uint32_t>(m_cameras.size());
    view.visibleItems.reserve(maximumObjectsPerFrame);
    view.visibleOutlined.reserve(maximumObjectsPerFrame);
    view.visibility.visibleInstances.reserve(maximumObjectsPerFrame);
    view.visibility.commands.reserve(maximumObjectsPerFrame);
    if (!view.clusteredLighting.init(context, frameCount, ClusterGridConfiguration{}, maximumLocalLights) ||
        !view.reflectionProbeClusters.init(context, frameCount, view.clusteredLighting.configuration()) ||
        !view.decalClusters.init(context, frameCount, view.clusteredLighting.configuration()) ||
        !view.volumetricFog.init(context, frameCount) ||
        !view.postProcess.init(context, m_outputFormat, m_sceneDepthFormat, frameCount, extent, depthView) ||
        !view.depthPyramid.init(context, extent, depthView) ||
        !view.ambientOcclusion.init(context, frameCount, extent) ||
        !view.transmissionBackground.init(context, extent)) {
        return false;
    }
    view.postProcess.setFixedFrameSeconds(m_fixedFrameSeconds);
    writeClusteredLightingDescriptors(view);
    writeReflectionProbeDescriptors(view);
    writeDecalDescriptors(view);
    view.fogDescriptorGenerations.assign(frameCount, 0);
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        writeVolumetricFogDescriptors(view, frame);
    }
    writeAmbientOcclusionDescriptors(view);
    writeTransmissionDescriptors(view);
    return true;
}

bool MeshPass::createView(const VulkanContext& context, uint32_t view, VkExtent2D extent, VkImageView depthView) {
    if (view == 0 || hasView(view)) {
        m_log.error("Render view slot " + std::to_string(view) + " cannot be created");
        return false;
    }
    if (view >= m_views.size()) {
        m_views.resize(view + 1);
    }
    auto state = std::make_unique<ViewState>();
    if (!createViewCameraSets(*state) || !initViewPasses(context, *state, extent, depthView)) {
        destroyViewState(context, *state);
        return false;
    }
    m_views[view] = std::move(state);
    return true;
}

bool MeshPass::resizeView(const VulkanContext& context, uint32_t view, VkExtent2D extent, VkImageView depthView) {
    if (!hasView(view)) {
        return false;
    }
    ViewState& state = *m_views[view];
    state.depthInitialized = false;
    state.cameraHistoryValid = false;
    state.temporalQuality.invalidateHistory();
    state.depthPrepassFrame.reset();
    if (!state.postProcess.resize(context, extent, depthView) || !state.depthPyramid.resize(extent, depthView) ||
        !state.ambientOcclusion.resize(extent) || !state.transmissionBackground.resize(extent)) {
        return false;
    }
    writeAmbientOcclusionDescriptors(state);
    writeTransmissionDescriptors(state);
    if (view == 0 && m_gpuDrivenSupported) {
        m_gpuDrivenPass.bindDepthPyramid(state.depthPyramid);
    }
    return true;
}

void MeshPass::destroyView(const VulkanContext& context, uint32_t view) {
    if (view == 0 || !hasView(view)) {
        return;
    }
    destroyViewState(context, *m_views[view]);
    m_views[view].reset();
}

bool MeshPass::createViewCameraSets(ViewState& view) {
    const uint32_t frameCount = static_cast<uint32_t>(m_cameras.size());
    const std::vector<VkDescriptorSetLayoutBinding> bindings = cameraLayoutBindings();
    const std::vector<VkDescriptorPoolSize> poolSizes = descriptorPoolSizes(bindings, frameCount);
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = frameCount,
                                              .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                              .pPoolSizes = poolSizes.data()};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &view.descriptorPool),
                     "Create render view descriptor pool")) {
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_cameraLayout);
    std::vector<VkDescriptorSet> sets(frameCount);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = view.descriptorPool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()),
                     "Allocate render view camera descriptor sets")) {
        return false;
    }
    view.cameras.resize(frameCount);
    const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                             .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        ViewCameraSet& camera = view.cameras[frame];
        camera.descriptor = sets[frame];
        VmaAllocationInfo info{};
        VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = sizeof(CameraData),
                                  .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &camera.uniform.buffer,
                                         &camera.uniform.allocation, &info),
                         "Create render view camera buffer")) {
            return false;
        }
        camera.uniformMapped = info.pMappedData;
        buffer.size = sizeof(ObjectData) * maximumObjectsPerFrame;
        buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &camera.objects.buffer,
                                         &camera.objects.allocation, &info),
                         "Create render view object buffer")) {
            return false;
        }
        camera.objectsMapped = info.pMappedData;
        const VkDescriptorBufferInfo uniformInfo{.buffer = camera.uniform.buffer, .range = sizeof(CameraData)};
        const VkDescriptorBufferInfo objectInfo{.buffer = camera.objects.buffer, .range = VK_WHOLE_SIZE};
        const std::array<VkWriteDescriptorSet, 2> writes{{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = camera.descriptor,
                                                           .dstBinding = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                           .pBufferInfo = &uniformInfo},
                                                          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = camera.descriptor,
                                                           .dstBinding = 2,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                           .pBufferInfo = &objectInfo}}};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return true;
}

void MeshPass::destroyViewState(const VulkanContext& context, ViewState& view) {
    // Through the release queue, after any atlas its local shadows retired.
    retireViewShadows(view);
    view.clusteredLighting.shutdown();
    view.reflectionProbeClusters.shutdown();
    view.decalClusters.shutdown();
    // The caller idled the device, so every retired fog volume may go.
    view.volumetricFog.collect(std::numeric_limits<uint64_t>::max());
    view.volumetricFog.shutdown();
    view.fogDescriptorGenerations.clear();
    view.postProcess.shutdown(context);
    view.depthPyramid.shutdown();
    view.ambientOcclusion.shutdown();
    view.transmissionBackground.shutdown();
    for (ViewCameraSet& camera : view.cameras) {
        if (camera.uniform.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.uniform.buffer, camera.uniform.allocation);
        }
        if (camera.objects.buffer) {
            vmaDestroyBuffer(context.vmaAllocator(), camera.objects.buffer, camera.objects.allocation);
        }
    }
    view.cameras.clear();
    if (view.descriptorPool) {
        vkDestroyDescriptorPool(context.device(), view.descriptorPool, nullptr);
    }
    view.descriptorPool = VK_NULL_HANDLE;
    view.depthPrepassFrame.reset();
    view.depthInitialized = false;
}

VkDescriptorSet MeshPass::viewCameraSet(const ViewState& view, uint32_t frameIndex) const {
    return view.sharesFrameSets ? m_cameras[frameIndex].descriptor : view.cameras[frameIndex].descriptor;
}

std::array<VkDescriptorSet, 2> MeshPass::viewCameraSets(const ViewState& view, uint32_t frameIndex) const {
    if (view.sharesFrameSets) {
        return {m_cameras[frameIndex].descriptor, m_cameras[frameIndex].gpuDrivenDescriptor};
    }
    return {view.cameras[frameIndex].descriptor, VK_NULL_HANDLE};
}

void MeshPass::copySharedCameraBindings(const ViewState& view, uint32_t frameIndex) {
    if (view.sharesFrameSets) {
        return;
    }

    struct SharedBinding {
        uint32_t binding;
        uint32_t count;
    };

    // The shadow bindings (1, the local-shadow atlas and tiles) come last, so
    // a view with its own shadow maps copies only the bindings before them.
    const SharedBinding shared[]{{3, 1},
                                 {4, 1},
                                 {EnvironmentPass::specularBinding, EnvironmentPass::environmentSlots},
                                 {EnvironmentPass::irradianceBinding, EnvironmentPass::environmentSlots},
                                 {EnvironmentPass::brdfBinding, 1},
                                 {IrradianceVolumePass::binding, 1},
                                 {previousSkinPaletteBinding, 1},
                                 {EnvironmentPass::probeSpecularBinding, 1},
                                 {1, 1},
                                 {LocalShadowPass::atlasBinding, 1},
                                 {LocalShadowPass::tileBinding, 1}};
    constexpr size_t shadowBindingCount = 3;
    std::array<VkCopyDescriptorSet, std::size(shared)> copies{};
    for (size_t index = 0; index < copies.size(); ++index) {
        copies[index] = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                         .srcSet = m_cameras[frameIndex].descriptor,
                         .srcBinding = shared[index].binding,
                         .dstSet = view.cameras[frameIndex].descriptor,
                         .dstBinding = shared[index].binding,
                         .descriptorCount = shared[index].count};
    }
    const size_t copyCount = view.shadows ? copies.size() - shadowBindingCount : copies.size();
    vkUpdateDescriptorSets(m_device, 0, nullptr, static_cast<uint32_t>(copyCount), copies.data());
    if (!view.shadows) {
        return;
    }
    // The view's own maps; rewritten every frame, since frames that shared
    // the frame's maps copied those bindings over them.
    const VkDescriptorImageInfo shadowMap{.sampler = m_shadowSampler,
                                          .imageView = view.shadows->directional.texture.view,
                                          .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = view.cameras[frameIndex].descriptor,
                                     .dstBinding = 1,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .pImageInfo = &shadowMap};
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    view.shadows->localShadows.writeDescriptors(frameIndex, {&view.cameras[frameIndex].descriptor, 1});
}
