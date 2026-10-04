#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

using namespace mesh_pass_detail;

bool MeshPass::recordPicks(const DrawContext& draw, const Scene& scene, const MeshPassViewRequest& request,
                           const ViewFrame& viewFrame) {
    // Pick regions address output pixels, so the id buffer uses the view's
    // camera unjittered at the output aspect, whatever the internal resolution.
    glm::mat4 projection = glm::perspectiveRH_ZO(
        viewFrame.verticalFov, static_cast<float>(request.extent.width) / static_cast<float>(request.extent.height),
        viewFrame.farPlane, viewFrame.nearPlane);
    projection[1][1] *= -1.0f;
    const glm::mat4 viewProjection = projection * viewFrame.view;
    // Pickable: the view's frustum-visible items, the list both CPU and
    // GPU-driven submission draw from (the latter culls further by occlusion,
    // which the id buffer's depth test replaces). Each entity gets one id, so
    // its primitives share it; id 0 means "no object".
    std::vector<const RenderItem*> items;
    std::vector<uint32_t> itemIds;
    std::vector<PickedObject> objects;
    std::unordered_map<Entity, uint32_t> entityIds;
    for (GpuSceneHandle handle : draw.viewState->visibility.visibleInstances) {
        const auto found = m_gpuHandleItems.find(handleKey(handle));
        if (found == m_gpuHandleItems.end()) {
            continue;
        }
        const RenderItem& item = m_renderItems[found->second];
        // A collection instance's draws pick as the instancing entity, and an
        // isolated view picks only what it draws.
        const Entity pickedEntity = item.renderObject();
        const RenderObjectId pickedObject = static_cast<RenderObjectId>(entt::to_integral(pickedEntity));
        if (request.visibleObjects != nullptr && !std::ranges::binary_search(*request.visibleObjects, pickedObject)) {
            continue;
        }
        const auto [entry, inserted] = entityIds.try_emplace(pickedEntity, static_cast<uint32_t>(objects.size() + 1));
        if (inserted) {
            objects.push_back({.entity = scene.id(pickedEntity), .object = pickedObject});
        }
        items.push_back(&item);
        itemIds.push_back(entry->second);
    }
    for (MeshPassPick& pick : request.picks) {
        if (pick.readback == nullptr) {
            m_log.error("A pick region has no readback");
            return false;
        }
        // A region whose targets cannot be allocated fails alone; the frame
        // still renders.
        if (!m_idBuffer.begin(draw.commandBuffer, m_frameSerial, request.extent, pick.region, draw.cameraSet)) {
            m_log.error("Cannot allocate the id buffer for a " + std::to_string(pick.region.width) + "x" +
                        std::to_string(pick.region.height) + " pick region");
            continue;
        }
        std::optional<uint32_t> boundPage;
        for (size_t index = 0; index < items.size(); ++index) {
            const RenderItem& item = *items[index];
            bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
            m_idBuffer.draw(draw.commandBuffer,
                            {.modelViewProjection = viewProjection * item.world,
                             .firstJoint = item.skinPaletteOffset,
                             .jointCount = static_cast<uint32_t>(item.jointPalette.size()),
                             .objectId = itemIds[index]},
                            item.lod.indexCount, item.gpuMesh->geometry.firstIndex + item.lod.firstIndex,
                            item.gpuMesh->geometry.vertexOffset);
        }
        m_idBuffer.end(draw.commandBuffer, *pick.readback);
        pick.recorded = true;
        pick.objects = objects;
        pick.viewProjection = viewProjection;
        ++draw.stats->pickPasses;
    }
    return true;
}

void MeshPass::recordSelectionMask(const DrawContext& draw) {
    if (std::ranges::none_of(draw.viewState->visibleOutlined, [](uint8_t outlined) { return outlined != 0; })) {
        return;
    }
    vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_selectionMaskPipeline.handle());
    vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_selectionMaskPipeline.layout(), 0, 1,
                            &draw.cameraSet, 0, nullptr);
    std::optional<uint32_t> boundPage;
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size(); ++itemIndex) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        if (draw.viewState->visibleOutlined[itemIndex] == 0) {
            continue;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, 1,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
    }
}

void MeshPass::recordOutlines(const DrawContext& draw) {
    if (std::ranges::none_of(draw.viewState->visibleOutlined, [](uint8_t outlined) { return outlined != 0; })) {
        return;
    }
    vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlinePipeline.handle());
    vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlinePipeline.layout(), 0, 1,
                            &draw.cameraSet, 0, nullptr);
    std::optional<uint32_t> boundPage;
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size(); ++itemIndex) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        if (draw.viewState->visibleOutlined[itemIndex] == 0) {
            continue;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        const glm::vec4 outlineSettings{item.mesh->bounds.center, 2.0f};
        vkCmdPushConstants(draw.commandBuffer, m_outlinePipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(outlineSettings), &outlineSettings);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, 1,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
    }
}

void MeshPass::recordWireframeOverlay(const DrawContext& draw, bool gpuDrivenActive) {
    if (!draw.pipelines->hasWireframeOverlay()) {
        return;
    }
    const OpaquePipelines overlay{.singleSided = &draw.pipelines->wireframe,
                                  .doubleSided = &draw.pipelines->wireframeDoubleSided};
    if (gpuDrivenActive) {
        // Both cull phases' compacted draws are complete inside the scene pass.
        recordGpuDrivenGroups(draw, GpuCullPhase::Early, overlay);
        recordGpuDrivenGroups(draw, GpuCullPhase::Late, overlay);
    }
    // CPU-submitted items: every surface without GPU culling, else the
    // blended ones (outlined opaque items are also listed then, but their
    // edges came from the indirect groups). Unlike recordShadedItems this adds
    // no visibility or timing statistics; the edges belong to surfaces already
    // counted.
    const VkDescriptorSet sets[]{draw.cameraSet, m_cameras[draw.frameIndex].bindlessDescriptor};
    const GraphicsPipeline* boundPipeline = nullptr;
    std::optional<uint32_t> boundPage;
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size(); ++itemIndex) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        if (gpuDrivenActive && !item.material->translucent()) {
            continue;
        }
        const GraphicsPipeline* pipeline = item.material->doubleSided ? overlay.doubleSided : overlay.singleSided;
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
            vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout(), 0,
                                    static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
            boundPipeline = pipeline;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, 1,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
    }
}
