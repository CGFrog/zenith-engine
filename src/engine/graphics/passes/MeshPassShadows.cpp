#include "../Lighting.hpp"
#include "../resources/DescriptorPoolSizes.hpp"
#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include "../vulkan/VulkanCheck.hpp"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <tuple>

using namespace mesh_pass_detail;

bool MeshPass::createShadowResources(VkFormat depthFormat) {
    m_shadowFormat = depthFormat;
    if (!createDirectionalShadowMap(m_shadowMap)) {
        return false;
    }
    VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_LINEAR,
                                .minFilter = VK_FILTER_LINEAR,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                .compareEnable = VK_TRUE,
                                .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                                .maxLod = 0.0f,
                                .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    if (!checkVulkan(vkCreateSampler(m_device, &sampler, nullptr, &m_shadowSampler), "Create shadow sampler")) {
        return false;
    }
    VkDescriptorImageInfo imageInfo{.sampler = m_shadowSampler,
                                    .imageView = m_shadowMap.texture.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    for (const CameraBuffer& camera : m_cameras) {
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = camera.descriptor,
                                   .dstBinding = 1,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &imageInfo};
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
    return true;
}

bool MeshPass::createDirectionalShadowMap(DirectionalShadowMap& map) {
    const VkFormat depthFormat = m_shadowFormat;
    VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = depthFormat,
                            .extent = {.width = shadowResolution, .height = shadowResolution, .depth = 1},
                            .mipLevels = 1,
                            .arrayLayers = maximumShadowCascades,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo allocation{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!checkVulkan(
            vmaCreateImage(m_allocator, &image, &allocation, &map.texture.image, &map.texture.allocation, nullptr),
            "Create shadow map")) {
        return false;
    }
    VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = map.texture.image,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                               .format = depthFormat,
                               .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                                    .baseMipLevel = 0,
                                                    .levelCount = 1,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = maximumShadowCascades}};
    if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &map.texture.view), "Create shadow map view")) {
        return false;
    }
    for (uint32_t cascade = 0; cascade < maximumShadowCascades; ++cascade) {
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange.baseArrayLayer = cascade;
        view.subresourceRange.layerCount = 1;
        if (!checkVulkan(vkCreateImageView(m_device, &view, nullptr, &map.layerViews[cascade]),
                         "Create shadow cascade view")) {
            return false;
        }
    }
    return true;
}

void MeshPass::destroyDirectionalShadowMap(DirectionalShadowMap& map) {
    for (VkImageView& view : map.layerViews) {
        if (view) {
            vkDestroyImageView(m_device, view, nullptr);
        }
    }
    if (map.texture.view) {
        vkDestroyImageView(m_device, map.texture.view, nullptr);
    }
    if (map.texture.image) {
        vmaDestroyImage(m_allocator, map.texture.image, map.texture.allocation);
    }
    map = {};
}

std::vector<int32_t> MeshPass::recordLocalShadows(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                                  const Scene& scene, const AssetManager& assets,
                                                  const RenderSettings& settings, std::span<const LocalLight> lights,
                                                  const glm::vec3& cameraPosition, OptionalPassTimestamps& timestamps,
                                                  RenderStats& stats) {
    const auto cpuBegin = std::chrono::steady_clock::now();
    const SceneRevisions& revisions = scene.revisions();
    collectShadowCasterChanges(scene);
    const bool enabled = settings.localLights != LocalLightingQuality::Disabled;
    const std::vector<RenderObjectId>* casterObjects = m_frameShadowCasters ? &*m_frameShadowCasters : nullptr;
    const LocalShadowFrameInput input{
        .quality = enabled ? settings.localShadows : LocalShadowQuality::Off,
        .lights = lights,
        .cameraPosition = cameraPosition,
        .changedCasters = m_changedShadowCasters,
        .invalidateAll = revisions.renderable != m_localShadowRevisions.renderable ||
                         revisions.material != m_localShadowRevisions.material ||
                         (m_hasCollectionInstances && revisions.transform != m_localShadowRevisions.transform) ||
                         assets.revision() != m_localShadowAssetRevision ||
                         m_localShadowCasterObjects != m_frameShadowCasters};
    const CameraBuffer& camera = m_cameras[frameIndex];
    const std::array<VkDescriptorSet, 2> cameraSets{camera.descriptor, camera.gpuDrivenDescriptor};
    const ShadowDrawTarget target = frameShadowTarget(frameIndex);
    m_localShadowObjectCount = 0;
    const std::vector<int32_t> firstTiles = m_localShadows.record(
        commandBuffer, frameIndex, m_frameSerial, input,
        std::span<const VkDescriptorSet>(cameraSets.data(), camera.gpuDrivenDescriptor ? 2u : 1u),
        camera.bindlessDescriptor, m_releaseQueue, timestamps,
        [&](VkCommandBuffer tileCommands, const glm::mat4& viewProjection) {
            return drawLocalShadowCasters(tileCommands, target, m_localShadowObjectCount, casterObjects, viewProjection,
                                          stats);
        },
        stats);
    m_localShadowRevisions = revisions;
    m_localShadowAssetRevision = assets.revision();
    m_localShadowCasterObjects = m_frameShadowCasters;
    stats.cpuLocalShadowMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuBegin).count();
    return firstTiles;
}

void MeshPass::collectShadowCasterChanges(const Scene& scene) {
    m_changedShadowCasters.clear();
    const auto addCaster = [&](const RenderItem& item) {
        if (item.material->translucent()) {
            return;
        }
        // updateRenderItems kept the previous transform, so both the vacated
        // and the newly covered space are reported.
        m_changedShadowCasters.push_back({item.boundsCenter, item.boundsRadius});
        m_changedShadowCasters.push_back({glm::vec3(item.previousWorld * glm::vec4(item.mesh->bounds.center, 1.0f)),
                                          item.mesh->bounds.radius * maximumAxisScale(item.previousWorld)});
    };
    const SceneRevisions& revisions = scene.revisions();
    if (revisions.transform != m_localShadowRevisions.transform) {
        for (Entity entity : scene.changedTransforms()) {
            const auto found = m_objectItems.find(static_cast<RenderObjectId>(entt::to_integral(entity)));
            if (found == m_objectItems.end()) {
                continue;
            }
            for (size_t itemIndex : found->second) {
                addCaster(m_renderItems[itemIndex]);
            }
        }
    }
    if (revisions.skin != m_localShadowRevisions.skin) {
        for (const RenderItem& item : m_renderItems) {
            if (!item.jointPalette.empty()) {
                addCaster(item);
            }
        }
    }
}

bool MeshPass::castsShadowFor(const RenderItem& item, const std::vector<RenderObjectId>* casterObjects) {
    return !item.material->translucent() &&
           (casterObjects == nullptr ||
            std::ranges::binary_search(*casterObjects,
                                       static_cast<RenderObjectId>(entt::to_integral(item.renderObject()))));
}

MeshPass::ShadowDrawTarget MeshPass::frameShadowTarget(uint32_t frameIndex) const {
    const CameraBuffer& camera = m_cameras[frameIndex];
    return {.cameraSet = camera.descriptor,
            .objectsMapped = camera.objectsMapped,
            .objectsAllocation = camera.objects.allocation,
            .localObjectBase = localShadowObjectBase};
}

bool MeshPass::drawLocalShadowCasters(VkCommandBuffer commandBuffer, const ShadowDrawTarget& target,
                                      uint32_t& objectCount, const std::vector<RenderObjectId>* casterObjects,
                                      const glm::mat4& viewProjection, RenderStats& stats) {
    const std::array<glm::vec4, 6> frustum = clipVolumePlanes(viewProjection);
    m_shadowVisibleItems.clear();
    for (const RenderItem& item : m_renderItems) {
        if (castsShadowFor(item, casterObjects) &&
            sphereReachesClipVolume(frustum, item.boundsCenter, item.boundsRadius)) {
            m_shadowVisibleItems.push_back(&item);
        }
    }
    if (objectCount + m_shadowVisibleItems.size() > maximumObjectsPerFrame) {
        return false;
    }
    // Keep each primitive's LOD range adjacent so its instances share a draw.
    std::ranges::stable_sort(m_shadowVisibleItems, {}, [](const RenderItem* item) {
        return std::tuple(item->material->doubleSided, handleKey(item->meshHandle), item->lod.firstIndex);
    });
    const uint32_t objectBase = target.localObjectBase + objectCount;
    auto* objectData = static_cast<ObjectData*>(target.objectsMapped);
    for (size_t index = 0; index < m_shadowVisibleItems.size(); ++index) {
        const RenderItem& item = *m_shadowVisibleItems[index];
        objectData[objectBase + index] = {.model = item.world,
                                          .previousModel = item.previousWorld,
                                          .skin = {item.skinPaletteOffset,
                                                   static_cast<uint32_t>(item.jointPalette.size()),
                                                   item.gpuMaterial->index, 0}};
    }
    if (!m_shadowVisibleItems.empty()) {
        vmaFlushAllocation(m_allocator, target.objectsAllocation, sizeof(ObjectData) * objectBase,
                           sizeof(ObjectData) * m_shadowVisibleItems.size());
    }
    objectCount += static_cast<uint32_t>(m_shadowVisibleItems.size());
    std::optional<uint32_t> boundPage;
    std::optional<bool> boundDoubleSided;
    for (size_t itemIndex = 0; itemIndex < m_shadowVisibleItems.size();) {
        const RenderItem& item = *m_shadowVisibleItems[itemIndex];
        size_t batchEnd = itemIndex + 1;
        while (batchEnd < m_shadowVisibleItems.size()) {
            const RenderItem& candidate = *m_shadowVisibleItems[batchEnd];
            if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                candidate.lod.indexCount != item.lod.indexCount ||
                candidate.material->doubleSided != item.material->doubleSided) {
                break;
            }
            ++batchEnd;
        }
        if (!boundDoubleSided || *boundDoubleSided != item.material->doubleSided) {
            vkCmdSetCullMode(commandBuffer, item.material->doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            boundDoubleSided = item.material->doubleSided;
        }
        bindGeometry(commandBuffer, *item.gpuMesh, boundPage);
        const uint32_t instanceCount = static_cast<uint32_t>(batchEnd - itemIndex);
        vkCmdDrawIndexed(commandBuffer, item.lod.indexCount, instanceCount,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         objectBase + static_cast<uint32_t>(itemIndex));
        stats.localShadowCasterCount += instanceCount;
        ++stats.drawCalls;
        stats.triangleCount += static_cast<uint64_t>(item.lod.indexCount / 3) * instanceCount;
        itemIndex = batchEnd;
    }
    return true;
}

std::array<bool, maximumShadowCascades> MeshPass::dirtyShadowCascades(
    const DirectionalShadowMap& map, const SceneRevisions& revisions, const std::vector<RenderObjectId>* casterObjects,
    const std::array<glm::mat4, maximumShadowCascades>& lightViewProjections, uint32_t cascadeCount) const {
    // ZENITH_DISABLE_SHADOW_CACHE redraws every cascade each frame, so shadow
    // rendering and culling can be measured on a static scene.
    const bool sameCasters =
        casterObjects == nullptr ? !map.casterObjects : map.casterObjects && *map.casterObjects == *casterObjects;
    const bool castersChanged =
        !map.initialized || SDL_getenv("ZENITH_DISABLE_SHADOW_CACHE") != nullptr ||
        revisions.transform != map.revisions.transform || revisions.renderable != map.revisions.renderable ||
        revisions.light != map.revisions.light || revisions.skin != map.revisions.skin ||
        revisions.material != map.revisions.material || m_sceneVisibilityMode != map.visibilityMode || !sameCasters;
    std::array<bool, maximumShadowCascades> dirtyCascades{};
    for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade) {
        float maximumDifference = 0.0f;
        for (uint32_t column = 0; column < 4; ++column) {
            for (uint32_t row = 0; row < 4; ++row) {
                maximumDifference =
                    std::max(maximumDifference, std::abs(lightViewProjections[cascade][column][row] -
                                                         map.lightViewProjections[cascade][column][row]));
            }
        }
        dirtyCascades[cascade] = castersChanged || cascade >= map.cascadeCount || maximumDifference > 0.0001f;
    }
    return dirtyCascades;
}

void MeshPass::rememberShadowCascades(DirectionalShadowMap& map, const SceneRevisions& revisions,
                                      const std::vector<RenderObjectId>* casterObjects,
                                      const std::array<glm::mat4, maximumShadowCascades>& lightViewProjections,
                                      uint32_t cascadeCount) const {
    map.revisions = revisions;
    map.visibilityMode = m_sceneVisibilityMode;
    if (casterObjects == nullptr) {
        map.casterObjects.reset();
    } else if (!map.casterObjects || *map.casterObjects != *casterObjects) {
        map.casterObjects = *casterObjects;
    }
    map.lightViewProjections = lightViewProjections;
    map.cascadeCount = cascadeCount;
}

bool MeshPass::recordShadow(VkCommandBuffer commandBuffer, DirectionalShadowMap& map, const ShadowDrawTarget& target,
                            uint32_t frameIndex, const std::vector<RenderItem>& items,
                            const DirectionalLightComponent& light,
                            const std::array<glm::mat4, maximumShadowCascades>& lightViewProjections,
                            uint32_t cascadeCount, const std::array<bool, maximumShadowCascades>& dirtyCascades,
                            ShadowCulling culling, const std::vector<RenderObjectId>* casterObjects,
                            const MeshLodView& lodView, OptionalPassTimestamps& passTimestamps, RenderStats& stats) {
    if (cascadeCount == 0 || std::ranges::none_of(dirtyCascades, [](bool dirty) { return dirty; })) {
        stats.shadowCascadesCached = cascadeCount;
        return true;
    }
    // Disabled light shadows still clear the dirty cascades, without casters.
    const bool drawCasters = light.shadows.enabled;
    if (culling == ShadowCulling::Gpu && drawCasters) {
        // All dirty cascades are culled up front: compute work cannot run
        // inside the cascades' rendering scopes.
        passTimestamps.write(FrameTimestamp::shadowCullBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade) {
            if (dirtyCascades[cascade]) {
                m_gpuDrivenPass.cullFrustumView(commandBuffer, frameIndex, cascade, lightViewProjections[cascade],
                                                lodView);
            }
        }
        passTimestamps.write(FrameTimestamp::shadowCullEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    }
    imageBarrier(commandBuffer, map.texture.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                 map.initialized ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 // Earlier frames sampled it in shading and volumetric fog.
                 map.initialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                 : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 map.initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, maximumShadowCascades);
    VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = VK_NULL_HANDLE,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}}};
    VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = {.width = shadowResolution, .height = shadowResolution}},
        .layerCount = 1,
        .pDepthAttachment = &depth};
    VkViewport viewport{.x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<float>(shadowResolution),
                        .height = static_cast<float>(shadowResolution),
                        .minDepth = 0.0f,
                        .maxDepth = 1.0f};
    VkRect2D scissor{.offset = {0, 0}, .extent = {.width = shadowResolution, .height = shadowResolution}};
    for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade) {
        if (!dirtyCascades[cascade]) {
            ++stats.shadowCascadesCached;
            continue;
        }
        ++stats.shadowCascadesRendered;
        depth.imageView = map.layerViews[cascade];
        vkCmdBeginRendering(commandBuffer, &rendering);
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline.handle());
        vkCmdSetDepthBias(commandBuffer, std::max(light.shadows.constantBias, 0.0f), 0.0f,
                          std::max(light.shadows.slopeBias, 0.0f));
        const glm::vec4 shadowPush{static_cast<float>(cascade), 0.0f, 0.0f, 0.0f};
        vkCmdPushConstants(commandBuffer, m_shadowPipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(shadowPush),
                           &shadowPush);
        if (culling == ShadowCulling::Gpu) {
            if (drawCasters) {
                recordGpuShadowCascade(commandBuffer, frameIndex, cascade, stats);
            }
            vkCmdEndRendering(commandBuffer);
            continue;
        }
        const VkDescriptorSet shadowSets[]{target.cameraSet, m_cameras[frameIndex].bindlessDescriptor};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline.layout(), 0,
                                static_cast<uint32_t>(std::size(shadowSets)), shadowSets, 0, nullptr);
        const std::array<glm::vec4, 6> shadowFrustum = clipVolumePlanes(lightViewProjections[cascade]);
        m_shadowVisibleItems.clear();
        m_shadowVisibleItems.reserve(items.size());
        for (const RenderItem& item : items) {
            if (light.shadows.enabled && castsShadowFor(item, casterObjects) &&
                sphereReachesClipVolume(shadowFrustum, item.boundsCenter, item.boundsRadius)) {
                m_shadowVisibleItems.push_back(&item);
            }
        }
        // Instances of one primitive may select different LODs; keep each
        // range adjacent so its instances still share a draw.
        std::ranges::stable_sort(m_shadowVisibleItems, {}, [](const RenderItem* item) {
            return std::tuple(item->material->doubleSided, handleKey(item->meshHandle), item->lod.firstIndex);
        });
        const uint32_t objectBase = cascade * maximumObjectsPerFrame;
        auto* objectData = static_cast<ObjectData*>(target.objectsMapped);
        for (size_t index = 0; index < m_shadowVisibleItems.size(); ++index) {
            objectData[objectBase + index].model = m_shadowVisibleItems[index]->world;
            objectData[objectBase + index].previousModel = m_shadowVisibleItems[index]->previousWorld;
            objectData[objectBase + index].skin = {
                m_shadowVisibleItems[index]->skinPaletteOffset,
                static_cast<uint32_t>(m_shadowVisibleItems[index]->jointPalette.size()),
                m_shadowVisibleItems[index]->gpuMaterial->index, 0};
        }
        vmaFlushAllocation(m_allocator, target.objectsAllocation, 0, VK_WHOLE_SIZE);
        std::optional<uint32_t> boundPage;
        std::optional<bool> boundDoubleSided;
        for (size_t itemIndex = 0; itemIndex < m_shadowVisibleItems.size();) {
            const RenderItem& item = *m_shadowVisibleItems[itemIndex];
            size_t batchEnd = itemIndex + 1;
            while (batchEnd < m_shadowVisibleItems.size()) {
                const RenderItem& candidate = *m_shadowVisibleItems[batchEnd];
                // Materials are per instance now, so only geometry and culling
                // state split a shadow batch.
                if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                    candidate.lod.indexCount != item.lod.indexCount ||
                    candidate.material->doubleSided != item.material->doubleSided) {
                    break;
                }
                ++batchEnd;
            }
            if (!boundDoubleSided || *boundDoubleSided != item.material->doubleSided) {
                vkCmdSetCullMode(commandBuffer, item.material->doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
                boundDoubleSided = item.material->doubleSided;
            }
            bindGeometry(commandBuffer, *item.gpuMesh, boundPage);
            const uint32_t instanceCount = static_cast<uint32_t>(batchEnd - itemIndex);
            vkCmdDrawIndexed(commandBuffer, item.lod.indexCount, instanceCount,
                             item.gpuMesh->geometry.firstIndex + item.lod.firstIndex,
                             item.gpuMesh->geometry.vertexOffset, objectBase + static_cast<uint32_t>(itemIndex));
            stats.shadowCasterCount += instanceCount;
            ++stats.drawCalls;
            stats.triangleCount += static_cast<uint64_t>(item.lod.indexCount / 3) * instanceCount;
            itemIndex = batchEnd;
        }
        vkCmdEndRendering(commandBuffer);
    }
    imageBarrier(commandBuffer, map.texture.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 maximumShadowCascades);
    map.initialized = true;
    return true;
}

void MeshPass::recordGpuShadowCascade(VkCommandBuffer commandBuffer, uint32_t frameIndex, uint32_t cascade,
                                      RenderStats& stats) {
    // The GPU-driven set binds the cull output as the object buffer, so
    // shadow.vert reads each instance by gl_InstanceIndex on both paths.
    const VkDescriptorSet shadowSets[]{m_cameras[frameIndex].gpuDrivenDescriptor,
                                       m_cameras[frameIndex].bindlessDescriptor};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline.layout(), 0,
                            static_cast<uint32_t>(std::size(shadowSets)), shadowSets, 0, nullptr);
    std::optional<bool> boundDoubleSided;
    for (uint32_t group = 0; group < m_drawPlan.groups.size(); ++group) {
        const IndirectDrawGroup& drawGroup = m_drawPlan.groups[group];
        const bool doubleSided = drawGroup.pipeline == doubleSidedBucket;
        if (!boundDoubleSided || *boundDoubleSided != doubleSided) {
            vkCmdSetCullMode(commandBuffer, doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            boundDoubleSided = doubleSided;
        }
        bindGeometryPage(commandBuffer, drawGroup.geometryPage);
        m_gpuDrivenPass.drawGroup(commandBuffer, GpuCullRegion::frustumView(cascade), group, drawGroup.firstBatch,
                                  drawGroup.batchCount);
        ++stats.drawCalls;
    }
}

std::unique_ptr<MeshPass::ViewShadows> MeshPass::createViewShadows() {
    auto shadows = std::make_unique<ViewShadows>();
    const uint32_t frameCount = static_cast<uint32_t>(m_cameras.size());
    const auto fail = [&] {
        destroyViewShadows(*shadows);
        return nullptr;
    };
    if (!createDirectionalShadowMap(shadows->directional) ||
        !shadows->localShadows.init(*m_context, frameCount, m_shadowFormat, m_cameraLayout, m_bindlessLayout)) {
        return fail();
    }
    const std::vector<VkDescriptorPoolSize> poolSizes = descriptorPoolSizes(cameraLayoutBindings(), frameCount);
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = frameCount,
                                              .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                              .pPoolSizes = poolSizes.data()};
    if (!checkVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &shadows->descriptorPool),
                     "Create view shadow descriptor pool")) {
        return fail();
    }
    const std::vector<VkDescriptorSetLayout> layouts(frameCount, m_cameraLayout);
    std::vector<VkDescriptorSet> sets(frameCount);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = shadows->descriptorPool,
                                               .descriptorSetCount = frameCount,
                                               .pSetLayouts = layouts.data()};
    if (!checkVulkan(vkAllocateDescriptorSets(m_device, &allocate, sets.data()),
                     "Allocate view shadow descriptor sets")) {
        return fail();
    }
    const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                             .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    // One region per cascade, then the local-light tiles' region.
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = sizeof(ObjectData) * maximumObjectsPerFrame * (maximumShadowCascades + 1),
                                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    shadows->frames.resize(frameCount);
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        ViewShadowFrame& shadowFrame = shadows->frames[frame];
        shadowFrame.cameraSet = sets[frame];
        VmaAllocationInfo info{};
        if (!checkVulkan(vmaCreateBuffer(m_allocator, &buffer, &allocation, &shadowFrame.objects.buffer,
                                         &shadowFrame.objects.allocation, &info),
                         "Create view shadow object buffer")) {
            return fail();
        }
        shadowFrame.objectsMapped = info.pMappedData;
        // Binding 0 is the frame's camera buffer, whose light matrices every
        // view's cascades share; skin palettes and materials are the frame's.
        const CameraBuffer& camera = m_cameras[frame];
        const VkDescriptorBufferInfo uniformInfo{.buffer = camera.buffer.buffer, .range = sizeof(CameraData)};
        const VkDescriptorBufferInfo objectInfo{.buffer = shadowFrame.objects.buffer, .range = VK_WHOLE_SIZE};
        const std::array<VkWriteDescriptorSet, 2> writes{{{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = shadowFrame.cameraSet,
                                                           .dstBinding = 0,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                           .pBufferInfo = &uniformInfo},
                                                          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                           .dstSet = shadowFrame.cameraSet,
                                                           .dstBinding = 2,
                                                           .descriptorCount = 1,
                                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                           .pBufferInfo = &objectInfo}}};
        std::array<VkCopyDescriptorSet, 3> copies{};
        const uint32_t copied[]{3, 4, previousSkinPaletteBinding};
        for (size_t index = 0; index < copies.size(); ++index) {
            copies[index] = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                             .srcSet = camera.descriptor,
                             .srcBinding = copied[index],
                             .dstSet = shadowFrame.cameraSet,
                             .dstBinding = copied[index],
                             .descriptorCount = 1};
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(),
                               static_cast<uint32_t>(copies.size()), copies.data());
    }
    return shadows;
}

void MeshPass::destroyViewShadows(ViewShadows& shadows) {
    destroyDirectionalShadowMap(shadows.directional);
    shadows.localShadows.shutdown();
    for (ViewShadowFrame& frame : shadows.frames) {
        if (frame.objects.buffer) {
            vmaDestroyBuffer(m_allocator, frame.objects.buffer, frame.objects.allocation);
        }
    }
    shadows.frames.clear();
    if (shadows.descriptorPool) {
        vkDestroyDescriptorPool(m_device, shadows.descriptorPool, nullptr);
    }
    shadows.descriptorPool = VK_NULL_HANDLE;
}

void MeshPass::retireViewShadows(ViewState& view) {
    if (!view.shadows) {
        return;
    }
    // std::function needs a copyable callable.
    std::shared_ptr<ViewShadows> retired(std::move(view.shadows));
    m_releaseQueue.defer(m_frameSerial, [this, retired] { destroyViewShadows(*retired); });
}

bool MeshPass::updateViewShadows(VkCommandBuffer commandBuffer, uint32_t frameIndex, ViewState& view,
                                 const MeshPassViewRequest& request, const FrameState& frame,
                                 std::span<const LocalLight> lights, const glm::vec3& cameraPosition,
                                 const MeshLodView& lodView, const Scene& scene, const AssetManager& assets,
                                 std::vector<int32_t>& shadowTiles, RenderStats& stats) {
    const std::vector<RenderObjectId>* casterObjects = request.visibleObjects;
    const bool sameCasters = casterObjects == nullptr ? !m_frameShadowCasters
                                                      : m_frameShadowCasters && *m_frameShadowCasters == *casterObjects;
    const RenderSettings& settings = *request.settings;
    const bool localShadows = frame.localLightLimit > 0 && settings.localShadows != LocalShadowQuality::Off;
    // A view drawing through the frame's sets defines the frame's casters.
    if (sameCasters || view.sharesFrameSets || (!frame.shadowsEnabled && !localShadows)) {
        retireViewShadows(view);
        return true;
    }
    if (!view.shadows) {
        view.shadows = createViewShadows();
        if (!view.shadows) {
            m_log.error("Cannot create the shadow maps of an isolated render view");
            return false;
        }
    }
    ViewShadows& shadows = *view.shadows;
    const ShadowDrawTarget target{.cameraSet = shadows.frames[frameIndex].cameraSet,
                                  .objectsMapped = shadows.frames[frameIndex].objectsMapped,
                                  .objectsAllocation = shadows.frames[frameIndex].objects.allocation,
                                  .localObjectBase = maximumShadowCascades * maximumObjectsPerFrame};
    // Only the leading view writes timestamps, and those cover the frame's maps.
    OptionalPassTimestamps noTimestamps(commandBuffer, VK_NULL_HANDLE);
    const SceneRevisions& revisions = scene.revisions();

    // Local lights: the frame stage collected this frame's caster changes,
    // so a view that skipped a frame redraws every tile.
    const bool localCastersChanged = casterObjects == nullptr
                                         ? shadows.localCasterObjects.has_value()
                                         : !shadows.localCasterObjects || *shadows.localCasterObjects != *casterObjects;
    const LocalShadowFrameInput input{
        .quality = localShadows ? settings.localShadows : LocalShadowQuality::Off,
        .lights = lights,
        .cameraPosition = cameraPosition,
        .changedCasters = m_changedShadowCasters,
        .invalidateAll = shadows.recordedSerial + 1 != m_frameSerial || localCastersChanged ||
                         revisions.renderable != shadows.localRevisions.renderable ||
                         revisions.material != shadows.localRevisions.material ||
                         (m_hasCollectionInstances && revisions.transform != shadows.localRevisions.transform) ||
                         assets.revision() != shadows.localAssetRevision};
    uint32_t localObjectCount = 0;
    shadowTiles = shadows.localShadows.record(
        commandBuffer, frameIndex, m_frameSerial, input, std::span<const VkDescriptorSet>(&target.cameraSet, 1),
        m_cameras[frameIndex].bindlessDescriptor, m_releaseQueue, noTimestamps,
        [&](VkCommandBuffer tileCommands, const glm::mat4& viewProjection) {
            return drawLocalShadowCasters(tileCommands, target, localObjectCount, casterObjects, viewProjection, stats);
        },
        stats);
    shadows.localRevisions = revisions;
    shadows.localAssetRevision = assets.revision();
    if (casterObjects == nullptr) {
        shadows.localCasterObjects.reset();
    } else if (localCastersChanged) {
        shadows.localCasterObjects = *casterObjects;
    }
    shadows.recordedSerial = m_frameSerial;

    // Sun cascades placed like the frame's, so the camera buffer's light
    // matrices and splits hold for both.
    DirectionalLightComponent shadowLight = frame.light;
    shadowLight.shadows.enabled = frame.shadowsEnabled;
    const std::array<bool, maximumShadowCascades> dirtyCascades = dirtyShadowCascades(
        shadows.directional, revisions, casterObjects, frame.base.lightViewProjections, frame.cascadeCount);
    if (!recordShadow(commandBuffer, shadows.directional, target, frameIndex, m_renderItems, shadowLight,
                      frame.base.lightViewProjections, frame.cascadeCount, dirtyCascades, ShadowCulling::Cpu,
                      casterObjects, lodView, noTimestamps, stats)) {
        return false;
    }
    rememberShadowCascades(shadows.directional, revisions, casterObjects, frame.base.lightViewProjections,
                           frame.cascadeCount);
    // Shading binds the map even when shadows are off and nothing was drawn.
    prepareShadowMapForSampling(commandBuffer, shadows.directional);
    stats.shadowCacheBytes =
        static_cast<uint64_t>(shadowResolution) * shadowResolution * sizeof(float) * maximumShadowCascades;
    return true;
}

void MeshPass::prepareShadowMapForSampling(VkCommandBuffer commandBuffer, DirectionalShadowMap& map) {
    if (map.initialized) {
        return;
    }
    // Fog and particles bind the shadow map even when no cascade was ever
    // rendered; they sample it only for rendered cascades.
    imageBarrier(commandBuffer, map.texture.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, maximumShadowCascades);
    map.initialized = true;
}

VkImageView MeshPass::viewShadowMap(const ViewState& view) const {
    return view.shadows ? view.shadows->directional.texture.view : m_shadowMap.texture.view;
}
