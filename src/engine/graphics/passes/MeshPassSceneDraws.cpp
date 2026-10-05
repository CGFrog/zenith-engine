#include "../DepthConvention.hpp"
#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <tuple>

using namespace mesh_pass_detail;

void MeshPass::beginRendering(VkCommandBuffer commandBuffer, VkImageView colorView, VkImageView motionView,
                              VkImage depthImage, VkImageView depthView, VkExtent2D extent,
                              SceneAttachments attachments, bool depthInitialized) const {
    const bool clear = attachments == SceneAttachments::Clear;
    // A preserved pass continues one the caller already transitioned.
    if (clear) {
        prepareDepthForClear(commandBuffer, depthImage, depthInitialized);
    }
    const VkAttachmentLoadOp loadOp =
        attachments == SceneAttachments::Preserve ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    const VkAttachmentLoadOp depthLoadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = colorView,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = loadOp,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.color = {.float32 = {0.015f, 0.02f, 0.04f, 0.0f}}}};
    VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = depthView,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    .loadOp = depthLoadOp,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.depthStencil = {.depth = scene_depth::clearValue, .stencil = 0}}};
    VkRenderingAttachmentInfo motion{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                     .imageView = motionView,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = loadOp,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                     .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}};
    const VkRenderingAttachmentInfo colors[]{color, motion};
    VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {.offset = {0, 0}, .extent = extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 2,
                              .pColorAttachments = colors,
                              .pDepthAttachment = &depth};
    vkCmdBeginRendering(commandBuffer, &rendering);
}

void MeshPass::prepareDepthForClear(VkCommandBuffer commandBuffer, VkImage depthImage, bool depthInitialized) const {
    // Last frame's post-processing sampled the depth it stored.
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                 depthInitialized ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 depthInitialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 depthInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

void MeshPass::beginDepthPrepass(VkCommandBuffer commandBuffer, VkImageView normalView, VkImageView specularView,
                                 VkImage depthImage, VkImageView depthView, VkExtent2D extent,
                                 SceneAttachments attachments, bool depthInitialized) const {
    const bool clear = attachments == SceneAttachments::Clear;
    if (clear) {
        prepareDepthForClear(commandBuffer, depthImage, depthInitialized);
    }
    const VkAttachmentLoadOp loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    const VkRenderingAttachmentInfo colors[]{{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = normalView,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = loadOp,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}},
                                             // A null view discards the shader's specular output.
                                             {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = specularView,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = loadOp,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}}};
    const VkRenderingAttachmentInfo depth{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depthView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = loadOp,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {.depth = scene_depth::clearValue, .stencil = 0}}};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .renderArea = {.offset = {0, 0}, .extent = extent},
                                    .layerCount = 1,
                                    .colorAttachmentCount = static_cast<uint32_t>(std::size(colors)),
                                    .pColorAttachments = colors,
                                    .pDepthAttachment = &depth};
    vkCmdBeginRendering(commandBuffer, &rendering);
}

void MeshPass::setSceneViewport(const DrawContext& draw) const {
    const VkViewport viewport{.x = 0,
                              .y = 0,
                              .width = static_cast<float>(draw.renderExtent.width),
                              .height = static_cast<float>(draw.renderExtent.height),
                              .minDepth = 0,
                              .maxDepth = 1};
    const VkRect2D scissor{.offset = {0, 0}, .extent = draw.renderExtent};
    vkCmdSetViewport(draw.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(draw.commandBuffer, 0, 1, &scissor);
}

void MeshPass::bindOpaquePipeline(const DrawContext& draw, const GraphicsPipeline& pipeline, OpaquePipelines pipelines,
                                  VkDescriptorSet cameraSet) const {
    const VkDescriptorSet sets[]{cameraSet, m_cameras[draw.frameIndex].bindlessDescriptor};
    vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0,
                            static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
    if (pipelines.pushesView) {
        vkCmdPushConstants(draw.commandBuffer, pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(draw.view),
                           &draw.view);
    }
}

bool MeshPass::recordDepthPrepass(const DrawContext& draw, bool gpuDrivenActive, VkImage depthImage,
                                  VkImageView depthView, const glm::mat4& viewProjection, const MeshLodView& lodView) {
    const VkCommandBuffer commandBuffer = draw.commandBuffer;
    const OpaquePipelines depthNormal{
        .singleSided = &m_depthNormalPipeline, .doubleSided = &m_depthNormalDoubleSidedPipeline, .pushesView = true};
    draw.viewState->postProcess.prepareNormals(commandBuffer, draw.frameIndex);
    // Only screen-space reflections read specular reflectance, but the
    // depth-normal pipelines always declare the attachment: a null view with a
    // non-UNDEFINED pipeline format needs dynamicRenderingUnusedAttachments,
    // so the target is written whenever the prepass runs.
    if (!draw.viewState->postProcess.prepareSpecular(commandBuffer, draw.frameIndex)) {
        return false;
    }
    const VkImageView normalView = draw.viewState->postProcess.normalView(draw.frameIndex);
    const VkImageView specularView = draw.viewState->postProcess.specularView(draw.frameIndex);
    beginDepthPrepass(commandBuffer, normalView, specularView, depthImage, depthView, draw.renderExtent,
                      SceneAttachments::Clear, draw.viewState->depthInitialized);
    setSceneViewport(draw);
    if (gpuDrivenActive) {
        // Two-phase occlusion culling runs here, so the shading pass reuses
        // both phases' compacted draws.
        recordGpuDrivenGroups(draw, GpuCullPhase::Early, depthNormal);
        vkCmdEndRendering(commandBuffer);
        prepareDepthForPyramid(commandBuffer, depthImage);
        draw.viewState->depthPyramid.build(commandBuffer, draw.renderExtent);
        m_gpuDrivenPass.cull(commandBuffer, draw.frameIndex, GpuCullPhase::Late, viewProjection,
                             draw.viewState->depthPyramid, lodView);
        restoreDepthAfterPyramid(commandBuffer, depthImage);
        beginDepthPrepass(commandBuffer, normalView, specularView, depthImage, depthView, draw.renderExtent,
                          SceneAttachments::Preserve, false);
        setSceneViewport(draw);
        recordGpuDrivenGroups(draw, GpuCullPhase::Late, depthNormal);
    } else {
        recordDepthNormalItems(draw);
    }
    vkCmdEndRendering(commandBuffer);
    draw.viewState->postProcess.finishNormals(commandBuffer, draw.frameIndex);
    draw.passTimestamps->write(FrameTimestamp::depthPrepassEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    // Screen-space consumers get the pyramid of the complete opaque depth.
    // With GPU-driven culling the occlusion build above already covers it
    // unless the late phase drew something, so the GPU skips this rebuild
    // when the late phase emitted nothing (the steady state): the pyramid is
    // then built once per frame. Late-drawn objects must reach the consumers,
    // so the rebuild cannot be dropped outright, and the late cull cannot use
    // last frame's pyramid instead, since objects disoccluded by camera motion
    // would be culled for a frame.
    draw.passTimestamps->write(FrameTimestamp::depthPyramidBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    prepareDepthForPyramid(commandBuffer, depthImage);
    draw.viewState->depthPyramid.build(commandBuffer, draw.renderExtent,
                                       gpuDrivenActive ? std::optional(m_gpuDrivenPass.lateEmittedCount())
                                                       : std::nullopt);
    draw.passTimestamps->write(FrameTimestamp::depthPyramidEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    restoreDepthAfterPyramid(commandBuffer, depthImage);
    return true;
}

void MeshPass::recordDepthNormalItems(const DrawContext& draw) {
    const OpaquePipelines depthNormal{
        .singleSided = &m_depthNormalPipeline, .doubleSided = &m_depthNormalDoubleSidedPipeline, .pushesView = true};
    const GraphicsPipeline* boundPipeline = nullptr;
    std::optional<uint32_t> boundPage;
    // Opaque items precede blended ones (collectCpuSubmittedItems).
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size();) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        if (item.material->translucent()) {
            break;
        }
        size_t batchEnd = itemIndex + 1;
        while (batchEnd < draw.viewState->visibleItems.size()) {
            const RenderItem& candidate = *draw.viewState->visibleItems[batchEnd];
            // The shading pass must test EQUAL against the same LOD's depth.
            if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                candidate.lod.indexCount != item.lod.indexCount || candidate.material->translucent() ||
                candidate.material->doubleSided != item.material->doubleSided) {
                break;
            }
            ++batchEnd;
        }
        const GraphicsPipeline& pipeline =
            item.material->doubleSided ? m_depthNormalDoubleSidedPipeline : m_depthNormalPipeline;
        if (&pipeline != boundPipeline) {
            bindOpaquePipeline(draw, pipeline, depthNormal, draw.cameraSet);
            boundPipeline = &pipeline;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, static_cast<uint32_t>(batchEnd - itemIndex),
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
        itemIndex = batchEnd;
    }
}

void MeshPass::prepareDepthForPyramid(VkCommandBuffer commandBuffer, VkImage depthImage) const {
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // The late pass loads the color targets the early pass stored.
    const VkMemoryBarrier2 colorBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &colorBarrier};
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void MeshPass::restoreDepthAfterPyramid(VkCommandBuffer commandBuffer, VkImage depthImage) const {
    imageBarrier(commandBuffer, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

bool MeshPass::collectCpuSubmittedItems(ViewState& view, bool gpuDrivenActive, const glm::vec3& cameraPosition,
                                        std::span<const RenderObjectId> outlinedObjects,
                                        const std::vector<RenderObjectId>* visibleObjects) {
    const auto objectId = [](const RenderItem& item) {
        return static_cast<RenderObjectId>(entt::to_integral(item.renderObject()));
    };
    // RenderItem::outlined marks every recorded view's outlines; this view
    // keeps only its own.
    const auto outlinedHere = [&](const RenderItem& item) {
        return item.outlined && std::ranges::binary_search(outlinedObjects, objectId(item));
    };
    view.visibleItems.clear();
    for (GpuSceneHandle handle : view.visibility.visibleInstances) {
        const auto found = m_gpuHandleItems.find(handleKey(handle));
        if (found == m_gpuHandleItems.end()) {
            continue;
        }
        const RenderItem& item = m_renderItems[found->second];
        if (visibleObjects != nullptr && !std::ranges::binary_search(*visibleObjects, objectId(item))) {
            continue;
        }
        // With GPU culling the CPU only submits what the indirect path cannot:
        // blended surfaces and the editor's outline passes.
        if (gpuDrivenActive && !item.material->translucent() && !outlinedHere(item)) {
            continue;
        }
        view.visibleItems.push_back(&item);
    }
    std::stable_sort(view.visibleItems.begin(), view.visibleItems.end(),
                     [&cameraPosition](const RenderItem* first, const RenderItem* second) {
                         const bool firstBlended = first->material->translucent();
                         const bool secondBlended = second->material->translucent();
                         if (firstBlended != secondBlended) {
                             return !firstBlended;
                         }
                         if (!firstBlended) {
                             // Opaque items arrive grouped by material, mesh and
                             // primitive; keeping each LOD's instances adjacent
                             // lets them share a draw.
                             return std::tuple(handleKey(first->materialHandle), handleKey(first->meshHandle),
                                               first->primitive->firstIndex, first->lod.firstIndex) <
                                    std::tuple(handleKey(second->materialHandle), handleKey(second->meshHandle),
                                               second->primitive->firstIndex, second->lod.firstIndex);
                         }
                         const glm::vec3 firstOffset = first->boundsCenter - cameraPosition;
                         const glm::vec3 secondOffset = second->boundsCenter - cameraPosition;
                         return glm::dot(firstOffset, firstOffset) > glm::dot(secondOffset, secondOffset);
                     });
    if (view.visibleItems.size() > maximumObjectsPerFrame) {
        m_log.error("Visible object count exceeds the per-frame render capacity");
        return false;
    }
    view.visibleOutlined.clear();
    for (const RenderItem* item : view.visibleItems) {
        view.visibleOutlined.push_back(outlinedHere(*item) ? 1u : 0u);
    }
    return true;
}

void MeshPass::uploadCpuSubmittedObjects(const DrawContext& draw) {
    const ViewState& view = *draw.viewState;
    void* mapped =
        view.sharesFrameSets ? m_cameras[draw.frameIndex].objectsMapped : view.cameras[draw.frameIndex].objectsMapped;
    const VmaAllocation allocation = view.sharesFrameSets ? m_cameras[draw.frameIndex].objects.allocation
                                                          : view.cameras[draw.frameIndex].objects.allocation;
    auto* objectData = static_cast<ObjectData*>(mapped) + draw.objectBase;
    for (size_t index = 0; index < view.visibleItems.size(); ++index) {
        const RenderItem& item = *view.visibleItems[index];
        objectData[index].model = item.world;
        objectData[index].previousModel = item.previousWorld;
        objectData[index].skin = {item.skinPaletteOffset, item.skinJointCount, item.gpuMaterial->index, 0};
    }
    vmaFlushAllocation(m_allocator, allocation, sizeof(ObjectData) * draw.objectBase,
                       sizeof(ObjectData) * std::max<size_t>(view.visibleItems.size(), 1));
}

void MeshPass::recordShadedItems(const DrawContext& draw, ShadedItems selection) {
    const VkDescriptorSet sets[]{draw.cameraSet, m_cameras[draw.frameIndex].bindlessDescriptor};
    const bool blendedOnly = selection == ShadedItems::BlendedOnly;
    const bool opaqueOnly = selection == ShadedItems::OpaqueOnly;
    const GraphicsPipeline* boundPipeline = nullptr;
    std::optional<uint32_t> boundPage;
    const auto opaqueCpuBegin = std::chrono::steady_clock::now();
    auto blendedCpuBegin = opaqueCpuBegin;
    // With GPU culling the opaque timestamp was already written after the
    // indirect draws.
    bool opaqueFinished = blendedOnly;
    for (size_t itemIndex = 0; itemIndex < draw.viewState->visibleItems.size();) {
        const RenderItem& item = *draw.viewState->visibleItems[itemIndex];
        const bool blended = item.material->translucent();
        if ((blendedOnly && !blended) || (opaqueOnly && blended)) {
            ++itemIndex;
            continue;
        }
        if (!opaqueFinished && blended) {
            if (draw.timestampQueries) {
                vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, draw.timestampQueries,
                                     2);
            }
            blendedCpuBegin = std::chrono::steady_clock::now();
            draw.stats->cpuOpaqueMilliseconds =
                std::chrono::duration<double, std::milli>(blendedCpuBegin - opaqueCpuBegin).count();
            opaqueFinished = true;
        }
        size_t batchEnd = itemIndex + 1;
        while (batchEnd < draw.viewState->visibleItems.size()) {
            const RenderItem& candidate = *draw.viewState->visibleItems[batchEnd];
            if (candidate.gpuMesh != item.gpuMesh || candidate.lod.firstIndex != item.lod.firstIndex ||
                candidate.lod.indexCount != item.lod.indexCount ||
                candidate.material->alphaMode != item.material->alphaMode ||
                candidate.material->translucent() != item.material->translucent() ||
                candidate.material->doubleSided != item.material->doubleSided) {
                break;
            }
            ++batchEnd;
        }
        const GraphicsPipeline* pipeline = nullptr;
        if (blended) {
            pipeline = item.material->doubleSided ? &draw.pipelines->blendDoubleSided : &draw.pipelines->blend;
        } else {
            pipeline = item.material->doubleSided ? &draw.pipelines->doubleSided : &draw.pipelines->opaque;
        }
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
            vkCmdBindDescriptorSets(draw.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout(), 0,
                                    static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);
            boundPipeline = pipeline;
        }
        bindGeometry(draw.commandBuffer, *item.gpuMesh, boundPage);
        const uint32_t instanceCount = static_cast<uint32_t>(batchEnd - itemIndex);
        vkCmdDrawIndexed(draw.commandBuffer, item.lod.indexCount, instanceCount,
                         item.gpuMesh->geometry.firstIndex + item.lod.firstIndex, item.gpuMesh->geometry.vertexOffset,
                         draw.objectBase + static_cast<uint32_t>(itemIndex));
        ++draw.stats->drawCalls;
        draw.stats->visibleCount += instanceCount;
        draw.stats->triangleCount += static_cast<uint64_t>(item.lod.indexCount / 3) * instanceCount;
        itemIndex = batchEnd;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!opaqueFinished) {
        if (draw.timestampQueries) {
            vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, draw.timestampQueries, 2);
        }
        draw.stats->cpuOpaqueMilliseconds = std::chrono::duration<double, std::milli>(now - opaqueCpuBegin).count();
    } else {
        draw.stats->cpuBlendedMilliseconds = std::chrono::duration<double, std::milli>(now - blendedCpuBegin).count();
    }
}
