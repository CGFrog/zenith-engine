#include "../Decals.hpp"
#include "../ReflectionProbes.hpp"
#include "../resources/DescriptorPoolSizes.hpp"
#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include "engine/graphics/MaterialRecord.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cstddef>

using namespace mesh_pass_detail;

namespace {
// Two slots (linear and sRGB view) per texture.
constexpr uint32_t maximumBindlessTextures = 8192;
// Slots kept free for the other sampled images bound alongside the array.
constexpr uint32_t reservedSampledImages = 16;
} // namespace

bool MeshPass::createGpuDrivenDescriptors() {
    std::vector<VkDescriptorSetLayout> layouts(m_cameras.size(), m_cameraLayout);
    std::vector<VkDescriptorSet> sets(m_cameras.size());
    VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = m_descriptorPool,
                                         .descriptorSetCount = static_cast<uint32_t>(sets.size()),
                                         .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()),
                     "Allocate GPU-driven camera descriptor sets")) {
        return false;
    }
    const VkDescriptorBufferInfo instances{.buffer = m_gpuDrivenPass.instanceBuffer(), .range = VK_WHOLE_SIZE};
    for (size_t frame = 0; frame < m_cameras.size(); ++frame) {
        CameraBuffer& camera = m_cameras[frame];
        camera.gpuDrivenDescriptor = sets[frame];
        // Everything except the object buffer matches the CPU-submission set.
        std::array<VkCopyDescriptorSet, 14> copies{};
        const uint32_t sharedBindings[]{0,
                                        1,
                                        3,
                                        4,
                                        9,
                                        10,
                                        11,
                                        previousSkinPaletteBinding,
                                        LocalShadowPass::atlasBinding,
                                        LocalShadowPass::tileBinding,
                                        ReflectionProbeClusterPass::cameraBinding,
                                        volumetricFogBinding,
                                        DecalClusterPass::recordsBinding,
                                        DecalClusterPass::clusterListsBinding};
        for (size_t index = 0; index < copies.size(); ++index) {
            copies[index] = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                             .srcSet = camera.descriptor,
                             .srcBinding = sharedBindings[index],
                             .dstSet = camera.gpuDrivenDescriptor,
                             .dstBinding = sharedBindings[index],
                             .descriptorCount = 1};
        }
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = camera.gpuDrivenDescriptor,
                                         .dstBinding = 2,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &instances};
        vkUpdateDescriptorSets(m_device, 1, &write, static_cast<uint32_t>(copies.size()), copies.data());
    }
    return true;
}

void MeshPass::writeClusteredLightingDescriptors(const ViewState& view) {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const ClusteredLightingPass::FrameBuffers buffers = view.clusteredLighting.frameBuffers(frame);
        // Bindings 9-11: local lights, per-cluster ranges, cluster light indices.
        const std::array<VkDescriptorBufferInfo, 3> infos{{{.buffer = buffers.lights, .range = VK_WHOLE_SIZE},
                                                           {.buffer = buffers.clusterRanges, .range = VK_WHOLE_SIZE},
                                                           {.buffer = buffers.lightIndices, .range = VK_WHOLE_SIZE}}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = viewCameraSet(view, frame),
                             .dstBinding = 9 + index,
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             .pBufferInfo = &infos[index]};
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void MeshPass::writeReflectionProbeDescriptors(const ViewState& view) {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const VkDescriptorBufferInfo probes{.buffer = view.reflectionProbeClusters.frameBuffer(frame),
                                            .range = VK_WHOLE_SIZE};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = viewCameraSet(view, frame),
                                         .dstBinding = ReflectionProbeClusterPass::cameraBinding,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &probes};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}

void MeshPass::writeDecalDescriptors(const ViewState& view) {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const DecalClusterPass::FrameBuffers buffers = view.decalClusters.frameBuffers(frame);
        const std::array<VkDescriptorBufferInfo, 2> infos{{{.buffer = buffers.records, .range = VK_WHOLE_SIZE},
                                                           {.buffer = buffers.clusterLists, .range = VK_WHOLE_SIZE}}};
        const std::array<uint32_t, 2> bindings{DecalClusterPass::recordsBinding, DecalClusterPass::clusterListsBinding};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t index = 0; index < writes.size(); ++index) {
            writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = viewCameraSet(view, frame),
                             .dstBinding = bindings[index],
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             .pBufferInfo = &infos[index]};
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

std::vector<GpuDecal> MeshPass::prepareDecals(const Scene& scene, const AssetManager& assets,
                                              const RenderSettings& settings, const glm::vec3& cameraPosition,
                                              const glm::mat4& view, uint32_t& sceneDecalCount) {
    sceneDecalCount = 0;
    if (settings.decals == DecalQuality::Off) {
        return {};
    }
    const std::vector<DecalProjector> sceneDecals = gatherDecals(scene, m_sceneVisibilityMode);
    sceneDecalCount = static_cast<uint32_t>(sceneDecals.size());
    // Textures resolve before the frame's bindless writes. A decal whose
    // texture is still uploading (or was rejected) is left out until it lands,
    // rather than drawing untextured. A colour slot samples through the sRGB
    // view unless the texture was imported as linear normal or data.
    const auto slotFor = [&](TextureHandle handle, bool colour) -> std::optional<uint32_t> {
        const GpuTexture* texture = handle ? ensureTexture(handle, assets) : nullptr;
        if (!texture) {
            return std::nullopt;
        }
        const TextureAsset* asset = assets.texture(handle);
        const bool srgb = colour && (asset == nullptr || asset->usage == TextureUsage::Color);
        return srgb ? texture->srgbSlot : texture->linearSlot;
    };
    std::vector<GpuDecal> records;
    for (const DecalProjector& decal : selectDecals(sceneDecals, cameraPosition, maximumDecals)) {
        const DecalTextureSlots slots{.albedo = slotFor(decal.albedoTexture, true),
                                      .normal = slotFor(decal.normalTexture, false)};
        if ((decal.albedoTexture && !slots.albedo) || (decal.normalTexture && !slots.normal)) {
            continue;
        }
        records.push_back(packDecal(decal, slots, view));
    }
    return records;
}

void MeshPass::writeAmbientOcclusionDescriptors(const ViewState& view) {
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        const VkDescriptorImageInfo occlusion{.sampler = view.ambientOcclusion.sampler(),
                                              .imageView = view.ambientOcclusion.occlusionView(frame),
                                              .imageLayout = AmbientOcclusionPass::occlusionLayout};
        // The GPU-driven set is written directly: copying would need the
        // CPU-submission set written first on every resize.
        std::array<VkWriteDescriptorSet, 2> writes{};
        uint32_t writeCount = 0;
        for (VkDescriptorSet set : viewCameraSets(view, frame)) {
            if (set == VK_NULL_HANDLE) {
                continue;
            }
            writes[writeCount++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                    .dstSet = set,
                                    .dstBinding = ambientOcclusionBinding,
                                    .descriptorCount = 1,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    .pImageInfo = &occlusion};
        }
        vkUpdateDescriptorSets(m_device, writeCount, writes.data(), 0, nullptr);
    }
}

void MeshPass::writeTransmissionBackgroundDescriptors(const ViewState& view) {
    const VkDescriptorImageInfo background{.sampler = view.transmissionBackground.sampler(),
                                           .imageView = view.transmissionBackground.view(),
                                           .imageLayout = TransmissionBackground::readLayout};
    for (uint32_t frame = 0; frame < m_cameras.size(); ++frame) {
        std::array<VkWriteDescriptorSet, 2> writes{};
        uint32_t writeCount = 0;
        for (VkDescriptorSet set : viewCameraSets(view, frame)) {
            if (set == VK_NULL_HANDLE) {
                continue;
            }
            writes[writeCount++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                    .dstSet = set,
                                    .dstBinding = transmissionBackgroundBinding,
                                    .descriptorCount = 1,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    .pImageInfo = &background};
        }
        vkUpdateDescriptorSets(m_device, writeCount, writes.data(), 0, nullptr);
    }
}

void MeshPass::writeVolumetricFogDescriptors(ViewState& view, uint32_t frameIndex) {
    if (view.fogDescriptorGenerations[frameIndex] == view.volumetricFog.generation()) {
        return;
    }
    const VkDescriptorImageInfo fog{.sampler = view.volumetricFog.sampler(),
                                    .imageView = view.volumetricFog.integratedView(),
                                    .imageLayout = VolumetricFogPass::volumeLayout};
    // The frame slot's previous submission retired, so its sets are not in use.
    std::array<VkWriteDescriptorSet, 2> writes{};
    uint32_t writeCount = 0;
    for (VkDescriptorSet set : viewCameraSets(view, frameIndex)) {
        if (set == VK_NULL_HANDLE) {
            continue;
        }
        writes[writeCount++] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .dstBinding = volumetricFogBinding,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &fog};
    }
    vkUpdateDescriptorSets(m_device, writeCount, writes.data(), 0, nullptr);
    view.fogDescriptorGenerations[frameIndex] = view.volumetricFog.generation();
}

bool MeshPass::createDescriptors(const VulkanContext& context, uint32_t frameCount) {
    const std::vector<VkDescriptorSetLayoutBinding> cameraBindings = cameraLayoutBindings();
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(std::size(cameraBindings)),
        .pBindings = cameraBindings.data(),
    };

    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_cameraLayout),
                     "Create camera descriptor layout")) {
        return false;
    }

    // Each frame has a CPU-submission camera set and a GPU-driven one that reads
    // instances from the culling output instead of the host object buffer.
    const uint32_t cameraSetCount = frameCount * 2;
    const std::vector<VkDescriptorPoolSize> poolSizes = descriptorPoolSizes(cameraBindings, cameraSetCount);
    VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = cameraSetCount,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };

    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool),
                     "Create camera descriptor pool")) {
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_cameraLayout);
    std::vector<VkDescriptorSet> sets(frameCount);
    VkDescriptorSetAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_descriptorPool,
        .descriptorSetCount = frameCount,
        .pSetLayouts = layouts.data(),
    };

    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()), "Allocate camera descriptor sets")) {
        return false;
    }

    m_cameras.resize(frameCount);

    for (uint32_t i = 0; i < frameCount; ++i) {
        VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = sizeof(CameraData),
                                  .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                           .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info{};

        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].buffer.buffer,
                                         &m_cameras[i].buffer.allocation, &info),
                         "Create camera buffer")) {
            return false;
        }

        m_cameras[i].mapped = info.pMappedData;
        m_cameras[i].descriptor = sets[i];
        VkDescriptorBufferInfo descriptorBuffer{
            .buffer = m_cameras[i].buffer.buffer, .offset = 0, .range = sizeof(CameraData)};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = sets[i],
                                   .dstBinding = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                   .pBufferInfo = &descriptorBuffer};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        // Binding 3 is a storage buffer in the layout and the shaders.
        buffer.size = sizeof(glm::mat4) * maximumSkinMatricesPerFrame;
        buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].skinPalettes.buffer,
                                         &m_cameras[i].skinPalettes.allocation, &info),
                         "Create per-frame skin palette buffer")) {
            return false;
        }
        m_cameras[i].skinPalettesMapped = info.pMappedData;
        VkDescriptorBufferInfo skinDescriptor{
            .buffer = m_cameras[i].skinPalettes.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        write.dstBinding = 3;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &skinDescriptor;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].previousSkinPalettes.buffer,
                                         &m_cameras[i].previousSkinPalettes.allocation, &info),
                         "Create per-frame previous skin palette buffer")) {
            return false;
        }
        m_cameras[i].previousSkinPalettesMapped = info.pMappedData;
        VkDescriptorBufferInfo previousSkinDescriptor{
            .buffer = m_cameras[i].previousSkinPalettes.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        VkWriteDescriptorSet previousSkinWrite = write;
        previousSkinWrite.dstBinding = previousSkinPaletteBinding;
        previousSkinWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        previousSkinWrite.pBufferInfo = &previousSkinDescriptor;
        vkUpdateDescriptorSets(m_device, 1, &previousSkinWrite, 0, nullptr);

        buffer.size = sizeof(ObjectData) * maximumObjectsPerFrame * objectRegionCount;
        buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].objects.buffer,
                                         &m_cameras[i].objects.allocation, &info),
                         "Create per-frame object buffer")) {
            return false;
        }
        m_cameras[i].objectsMapped = info.pMappedData;
        VkDescriptorBufferInfo objectDescriptor{
            .buffer = m_cameras[i].objects.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        write.dstBinding = 2;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &objectDescriptor;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        buffer.size = sizeof(MaterialRecord) * maximumMaterials;
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &m_cameras[i].materials.buffer,
                                         &m_cameras[i].materials.allocation, &info),
                         "Create per-frame material buffer")) {
            return false;
        }
        m_cameras[i].materialsMapped = info.pMappedData;
        VkDescriptorBufferInfo materialDescriptor{
            .buffer = m_cameras[i].materials.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        write.dstBinding = 4;
        write.pBufferInfo = &materialDescriptor;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context.pDevice(), &properties);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(context.pDevice(), &features);
    VkSamplerCreateInfo sampler{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = features.samplerAnisotropy,
        .maxAnisotropy = features.samplerAnisotropy ? std::min(8.0f, properties.limits.maxSamplerAnisotropy) : 1.0f,
        .maxLod = 16.0f};
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler), "Create material sampler")) {
        return false;
    }

    return true;
}

bool MeshPass::createBindlessDescriptors(const VulkanContext& context, uint32_t frameCount) {
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
    vkGetPhysicalDeviceFeatures2(context.pDevice(), &features);
    VkPhysicalDeviceVulkan12Properties properties12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                           .pNext = &properties12};
    vkGetPhysicalDeviceProperties2(context.pDevice(), &properties);
    // Update-after-bind is never used to modify live sets. It is requested only
    // when the ordinary sampled-image limits cannot hold the full table, since
    // it raises those limits on some vendors but also restricts tooling such as
    // frame capture. ZENITH_FORCE_UPDATE_AFTER_BIND takes that path on any
    // device that supports it, so it can be validated where it is not needed.
    const auto& limits = properties.properties.limits;
    const uint32_t ordinaryLimit =
        std::min(limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSampledImages);
    const uint32_t updateAfterBindLimit = std::min(properties12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                                   properties12.maxDescriptorSetUpdateAfterBindSampledImages);
    const bool forceUpdateAfterBind = SDL_getenv("ZENITH_FORCE_UPDATE_AFTER_BIND") != nullptr;
    const bool updateAfterBind =
        features12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
        (forceUpdateAfterBind ||
         (ordinaryLimit < maximumBindlessTextures + reservedSampledImages && updateAfterBindLimit > ordinaryLimit));
    if (forceUpdateAfterBind) {
        context.log().info(updateAfterBind ? "Bindless materials use update-after-bind (ZENITH_FORCE_UPDATE_AFTER_BIND)"
                                           : "ZENITH_FORCE_UPDATE_AFTER_BIND ignored: the device lacks sampled-image "
                                             "update-after-bind");
    }
    const uint32_t deviceLimit = updateAfterBind ? updateAfterBindLimit : ordinaryLimit;
    if (deviceLimit <= reservedSampledImages + 3) {
        context.log().error("The device exposes too few sampled-image descriptors for bindless materials");
        return false;
    }
    const uint32_t capacity = std::min(maximumBindlessTextures, deviceLimit - reservedSampledImages);

    const VkDescriptorSetLayoutBinding bindings[]{{.binding = 0,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                                   .descriptorCount = 1,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                   .pImmutableSamplers = &m_sampler},
                                                  {.binding = 1,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                   .descriptorCount = capacity,
                                                   .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    const VkDescriptorBindingFlags updateFlag = updateAfterBind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0;
    const VkDescriptorBindingFlags bindingFlags[]{0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                         VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                                         updateFlag};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(std::size(bindingFlags)),
        .pBindingFlags = bindingFlags};
    VkDescriptorSetLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .pNext = &flagsInfo,
                                               .flags = updateAfterBind
                                                            ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT
                                                            : VkDescriptorSetLayoutCreateFlags{0},
                                               .bindingCount = static_cast<uint32_t>(std::size(bindings)),
                                               .pBindings = bindings};
    if (!checkVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_bindlessLayout),
                     "Create bindless material layout")) {
        return false;
    }
    const VkDescriptorPoolSize poolSizes[]{
        {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = frameCount},
        {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = capacity * frameCount}};
    VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .flags = updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                                                                 : VkDescriptorPoolCreateFlags{0},
                                        .maxSets = frameCount,
                                        .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
                                        .pPoolSizes = poolSizes};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_bindlessPool),
                     "Create bindless material pool")) {
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_bindlessLayout);
    std::vector<uint32_t> counts(frameCount, capacity);
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableCounts{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = frameCount,
        .pDescriptorCounts = counts.data()};
    VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .pNext = &variableCounts,
                                         .descriptorPool = m_bindlessPool,
                                         .descriptorSetCount = frameCount,
                                         .pSetLayouts = layouts.data()};
    std::vector<VkDescriptorSet> sets(frameCount);
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()), "Allocate bindless material sets")) {
        return false;
    }
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        m_cameras[frame].bindlessDescriptor = sets[frame];
    }

    m_textureTable = std::make_unique<BindlessTextureTable>(capacity, frameCount, m_whiteTexture.view);
    const auto white = m_textureTable->acquire(m_whiteTexture.view);
    const auto whiteSrgb = m_textureTable->acquire(m_whiteTexture.srgbView);
    const auto normal = m_textureTable->acquire(m_normalTexture.view);
    if (!white || !whiteSrgb || !normal) {
        return false;
    }
    m_whiteSlot = *white;
    m_whiteSrgbSlot = *whiteSrgb;
    m_normalSlot = *normal;
    return true;
}
