#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include <cstddef>

using namespace mesh_pass_detail;

bool MeshPass::prepareGpuDrivenFrame(uint32_t frameIndex, const MeshLodView& lodView, uint32_t visibleInstanceCount) {
    m_gpuDrivenCandidates.clear();
    m_gpuDrivenKeys.clear();
    m_gpuDrivenKeySources.clear();
    // Without LOD selection the cull shader never leaves level 0, so skip the
    // coarser batches and their reserved instance slots.
    const bool lodEnabled = lodView.errorScale > 0.0f;
    for (const RenderItem& item : m_renderItems) {
        if (item.material->translucent()) {
            continue;
        }
        const GeometryAllocation& geometry = item.gpuMesh->geometry;
        const uint32_t candidate = static_cast<uint32_t>(m_gpuDrivenCandidates.size());
        m_gpuDrivenCandidates.push_back(&item);
        const uint32_t levelCount = lodEnabled ? item.primitive->lodCount() : 1u;
        for (uint32_t level = 0; level < levelCount; ++level) {
            const MeshLod lod = item.primitive->lod(level);
            m_gpuDrivenKeys.push_back({.pipeline = item.material->doubleSided ? doubleSidedBucket : singleSidedBucket,
                                       .geometryPage = geometry.page,
                                       .firstIndex = geometry.firstIndex + lod.firstIndex,
                                       .vertexOffset = geometry.vertexOffset,
                                       .indexCount = lod.indexCount});
            m_gpuDrivenKeySources.push_back({.candidate = candidate, .level = level});
        }
    }
    if (m_gpuDrivenKeys.size() > maximumGpuDrivenInstances) {
        m_log.error("GPU-driven LOD instances exceed the renderer capacity");
        return false;
    }
    buildIndirectDrawPlan(m_gpuDrivenKeys, m_drawPlan);
    if (m_drawPlan.groups.size() > maximumGpuDrawGroups) {
        m_log.error("GPU-driven draw groups exceed the renderer capacity");
        return false;
    }
    m_gpuDrivenBatches.clear();
    for (const IndirectDrawBatch& batch : m_drawPlan.batches) {
        m_gpuDrivenBatches.push_back({.indexCount = batch.key.indexCount,
                                      .firstIndex = batch.key.firstIndex,
                                      .vertexOffset = batch.key.vertexOffset,
                                      .firstInstance = batch.firstInstance,
                                      .group = batch.group,
                                      .groupFirstBatch = m_drawPlan.groups[batch.group].firstBatch});
    }
    m_gpuDrivenKeyBatches.resize(m_gpuDrivenKeys.size());
    for (size_t slot = 0; slot < m_drawPlan.instanceOrder.size(); ++slot) {
        m_gpuDrivenKeyBatches[m_drawPlan.instanceOrder[slot]] = m_drawPlan.instanceBatch[slot];
    }
    // Chain each primitive's levels so the cull shader can walk from level 0
    // to coarser batches. Every instance of a primitive writes the same links.
    for (size_t key = 0; key < m_gpuDrivenKeys.size(); ++key) {
        const GpuDrivenKeySource source = m_gpuDrivenKeySources[key];
        GpuDrivenBatch& batch = m_gpuDrivenBatches[m_gpuDrivenKeyBatches[key]];
        batch.lodGeometricError = m_gpuDrivenCandidates[source.candidate]->primitive->lod(source.level).geometricError;
        const bool hasCoarserLevel =
            key + 1 < m_gpuDrivenKeys.size() && m_gpuDrivenKeySources[key + 1].candidate == source.candidate;
        if (hasCoarserLevel) {
            batch.nextLodBatch = m_gpuDrivenKeyBatches[key + 1];
        }
    }
    m_gpuDrivenObjects.clear();
    for (size_t slot = 0; slot < m_drawPlan.instanceOrder.size(); ++slot) {
        const GpuDrivenKeySource source = m_gpuDrivenKeySources[m_drawPlan.instanceOrder[slot]];
        if (source.level != 0) {
            continue;
        }
        const RenderItem& item = *m_gpuDrivenCandidates[source.candidate];
        const uint32_t levelCount = lodEnabled ? item.primitive->lodCount() : 1u;
        // The persistent-scene slot is stable while the object lives, so it
        // carries the object's visibility from one frame to the next.
        m_gpuDrivenObjects.push_back(
            {.model = item.world,
             .previousModel = item.previousWorld,
             .bounds = {item.boundsCenter, item.boundsRadius},
             .draw = {m_drawPlan.instanceBatch[slot], item.gpuSceneHandle.index, levelCount, 0u},
             .skin = {item.skinPaletteOffset, item.skinJointCount, item.gpuMaterial->index, 0u}});
    }
    return m_gpuDrivenPass.prepare(frameIndex, m_gpuDrivenObjects, m_gpuDrivenBatches, visibleInstanceCount);
}

void MeshPass::recordGpuDrivenGroups(const DrawContext& draw, GpuCullPhase phase, OpaquePipelines pipelines) {
    const GraphicsPipeline* boundPipeline = nullptr;
    for (uint32_t group = 0; group < m_drawPlan.groups.size(); ++group) {
        const IndirectDrawGroup& drawGroup = m_drawPlan.groups[group];
        const GraphicsPipeline& pipeline =
            drawGroup.pipeline == doubleSidedBucket ? *pipelines.doubleSided : *pipelines.singleSided;
        if (&pipeline != boundPipeline) {
            bindOpaquePipeline(draw, pipeline, pipelines, m_cameras[draw.frameIndex].gpuDrivenDescriptor);
            boundPipeline = &pipeline;
        }
        bindGeometryPage(draw.commandBuffer, drawGroup.geometryPage);
        m_gpuDrivenPass.drawGroup(draw.commandBuffer, GpuCullRegion::phase(phase), group, drawGroup.firstBatch,
                                  drawGroup.batchCount);
        ++draw.stats->drawCalls;
    }
}
