#include "MeshPass.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

bool MeshPass::recordScreenSpaceReflections(const DrawContext& draw, VkImage depthImage, VkImageView depthView,
                                            ReflectionQuality reflections, const ScreenSpaceReflectionCamera& camera) {
    const SceneGeometryViews geometry = sceneGeometry(*draw.viewState, draw.frameIndex);
    const bool traces = PostProcessPass::tracesReflections(geometry, reflections);
    if (traces) {
        vkCmdEndRendering(draw.commandBuffer);
    }
    if (!draw.viewState->postProcess.recordReflections(draw.commandBuffer, draw.frameIndex, geometry, reflections,
                                                       camera, draw.timestampQueries)) {
        return false;
    }
    if (traces) {
        // The resumed pass loads the motion and depth attachments stored above;
        // recordReflections() ordered the HDR target itself.
        const VkMemoryBarrier2 attachments{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &attachments};
        vkCmdPipelineBarrier2(draw.commandBuffer, &dependency);
        beginRendering(draw.commandBuffer, draw.viewState->postProcess.sceneView(draw.frameIndex),
                       draw.viewState->postProcess.motionView(draw.frameIndex), depthImage, depthView,
                       draw.renderExtent, SceneAttachments::Preserve, false);
        setSceneViewport(draw);
    }
    return true;
}

void MeshPass::recordTransmissionBackground(const DrawContext& draw, VkImage depthImage, VkImageView depthView) {
    const bool transmissive = std::ranges::any_of(draw.viewState->visibleItems, [](const RenderItem* item) {
        return item->material->transmission.factor > 0.0f;
    });
    if (!transmissive) {
        return;
    }
    draw.passTimestamps->write(FrameTimestamp::transmissionBackgroundBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    vkCmdEndRendering(draw.commandBuffer);
    draw.viewState->transmissionBackground.capture(
        draw.commandBuffer, draw.viewState->postProcess.sceneImage(draw.frameIndex), draw.renderExtent);
    // The resumed pass loads the motion and depth attachments stored above;
    // capture() ordered the HDR target itself.
    const VkMemoryBarrier2 attachments{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &attachments};
    vkCmdPipelineBarrier2(draw.commandBuffer, &dependency);
    beginRendering(draw.commandBuffer, draw.viewState->postProcess.sceneView(draw.frameIndex),
                   draw.viewState->postProcess.motionView(draw.frameIndex), depthImage, depthView, draw.renderExtent,
                   SceneAttachments::Preserve, false);
    setSceneViewport(draw);
    draw.passTimestamps->write(FrameTimestamp::transmissionBackgroundEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

SceneGeometryViews MeshPass::sceneGeometry(const ViewState& view, uint32_t frameIndex) const {
    if (view.depthPrepassFrame != frameIndex) {
        return {};
    }
    return {.normals = view.postProcess.normalView(frameIndex),
            .specular = view.postProcess.specularView(frameIndex),
            .depthPyramid = view.depthPyramid.view(),
            .depthPyramidSampler = view.depthPyramid.sampler(),
            .depthPyramidLevels = view.depthPyramid.levelCount(),
            .renderExtent = view.depthPyramid.renderExtent(),
            .storageExtent = view.depthPyramid.storageExtent()};
}

void MeshPass::recordVolumetricFog(const DrawContext& draw, const VolumetricFogFrame* frame) {
    if (frame == nullptr) {
        // Keeps the bound volume free of fog; records nothing once cleared.
        draw.viewState->volumetricFog.recordCleared(draw.commandBuffer);
        return;
    }
    prepareShadowMapForSampling(draw.commandBuffer, m_shadowMap);
    draw.passTimestamps->write(FrameTimestamp::volumetricFogBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    draw.viewState->volumetricFog.record(draw.commandBuffer, draw.frameIndex, *frame);
    draw.passTimestamps->write(FrameTimestamp::volumetricFogEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

bool MeshPass::recordParticleSimulation(const DrawContext& draw, const Scene& scene, const AssetManager& assets,
                                        ParticleQuality quality, const ParticleSimulationFrame& frame) {
    const std::vector<particles::EmitterSnapshot> emitters =
        quality == ParticleQuality::Off ? std::vector<particles::EmitterSnapshot>{}
                                        : particles::gatherEmitters(scene, m_sceneVisibilityMode);
    // Sprite slot 0 is the procedural sprite; its view only keeps the
    // descriptor array valid. Emitters share a slot per distinct texture.
    m_particleSpriteViews.fill(m_whiteTexture.view);
    uint32_t usedSlots = 1;
    std::vector<uint32_t> textureSlots(emitters.size(), 0u);
    for (size_t index = 0; index < emitters.size(); ++index) {
        VkSampler sampler = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        const TextureHandle texture = emitters[index].emitter.texture;
        if (!texture || !uiTexture(texture, assets, sampler, view)) {
            continue;
        }
        const auto used = m_particleSpriteViews.begin() + usedSlots;
        const auto found = std::find(m_particleSpriteViews.begin() + 1, used, view);
        if (found != used) {
            textureSlots[index] = static_cast<uint32_t>(found - m_particleSpriteViews.begin());
        } else if (usedSlots < particles::maximumSpriteTextures) {
            m_particleSpriteViews[usedSlots] = view;
            textureSlots[index] = usedSlots++;
        }
    }
    if (!emitters.empty()) {
        prepareShadowMapForSampling(draw.commandBuffer, m_shadowMap);
    }
    draw.passTimestamps->write(FrameTimestamp::particleSimulationBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    const bool recorded = m_particles.recordSimulation(
        draw.commandBuffer, draw.frameIndex, m_frameSerial, quality,
        {.emitters = emitters, .textureSlots = textureSlots, .sceneGeneration = scene.contentGeneration()}, frame);
    draw.passTimestamps->write(FrameTimestamp::particleSimulationEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    draw.stats->particleEmitterCount = m_particles.emitterCount();
    draw.stats->particleCapacity = m_particles.capacity();
    draw.stats->droppedParticleEmitters = m_particles.droppedEmitters();
    if (m_particleEmitterOverflow.update(m_particles.droppedEmitters() > 0)) {
        m_log.warning("Particle emitters exceed the per-frame capacity of " +
                      std::to_string(particles::maximumEmitters) + "; " +
                      std::to_string(m_particles.droppedEmitters()) + " emitters are not simulated");
    }
    return recorded;
}

void MeshPass::recordParticles(const DrawContext& draw, VkImage depthImage, VkImageView depthView) {
    if (!m_particles.drawPending()) {
        return;
    }
    vkCmdEndRendering(draw.commandBuffer);
    draw.passTimestamps->write(FrameTimestamp::particleDrawBegin, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    m_particles.recordDraw(draw.commandBuffer, {.sceneColor = draw.viewState->postProcess.sceneView(draw.frameIndex),
                                                .motion = draw.viewState->postProcess.motionView(draw.frameIndex),
                                                .depthImage = depthImage,
                                                .depthView = depthView,
                                                .renderExtent = draw.renderExtent,
                                                .cameraSet = draw.cameraSet,
                                                .view = draw.view,
                                                .spriteViews = m_particleSpriteViews,
                                                .spriteSampler = m_sampler});
    draw.passTimestamps->write(FrameTimestamp::particleDrawEnd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    beginRendering(draw.commandBuffer, draw.viewState->postProcess.sceneView(draw.frameIndex),
                   draw.viewState->postProcess.motionView(draw.frameIndex), depthImage, depthView, draw.renderExtent,
                   SceneAttachments::Preserve, false);
    setSceneViewport(draw);
}

void MeshPass::recordAmbientOcclusion(const DrawContext& draw, AmbientOcclusionQuality quality,
                                      const glm::mat4& projection, bool temporalNoise) {
    if (draw.timestampQueries) {
        vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, draw.timestampQueries, 10);
    }
    const SceneGeometryViews geometry = sceneGeometry(*draw.viewState, draw.frameIndex);
    if (quality == AmbientOcclusionQuality::Off || !geometry.valid()) {
        draw.viewState->ambientOcclusion.recordUnoccluded(draw.commandBuffer, draw.frameIndex);
    } else {
        draw.viewState->ambientOcclusion.record(
            draw.commandBuffer, draw.frameIndex, quality,
            {.normals = geometry.normals,
             .depthPyramid = geometry.depthPyramid,
             .depthPyramidSampler = geometry.depthPyramidSampler,
             .renderExtent = geometry.renderExtent,
             .inverseProjection = glm::inverse(projection),
             .projectionScale = 0.5f * static_cast<float>(geometry.renderExtent.height) * std::abs(projection[1][1]),
             .temporalNoise = temporalNoise});
    }
    if (draw.timestampQueries) {
        vkCmdWriteTimestamp2(draw.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, draw.timestampQueries, 11);
    }
}
