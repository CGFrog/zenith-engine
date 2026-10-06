// MeshPass's per-scene render packets: the frame's scene and the secondary
// scenes created views draw instead (RenderRuntime::setViewScene). The
// per-scene members of MeshPass hold the scene being processed; a secondary
// scene's state is exchanged into them while its packets build and its views
// record. Secondary scenes are drawn with preview lighting: their views get a
// frame state without the frame scene's lights, shadows, environment maps,
// probes, irradiance volume, or fog, and draw no particles.

#include "MeshPassInternal.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

using namespace mesh_pass_detail;

void MeshPass::exchangeSceneState(SceneRenderState& state) {
    std::swap(m_renderItems, state.renderItems);
    std::swap(m_objectItems, state.objectItems);
    std::swap(m_gpuRecords, state.gpuRecords);
    std::swap(m_gpuHandleItems, state.gpuHandleItems);
    std::swap(m_gpuScene, state.gpuScene);
    std::swap(m_sceneRevisions, state.sceneRevisions);
    std::swap(m_assetRevision, state.assetRevision);
    std::swap(m_outlineRevision, state.outlineRevision);
    std::swap(m_outlinedObjects, state.outlinedObjects);
    std::swap(m_builtVisibilityMode, state.builtVisibilityMode);
    std::swap(m_hasCollectionInstances, state.hasCollectionInstances);
    std::swap(m_collectionInstanceDraws, state.collectionInstanceDraws);
    std::swap(m_shadowVisibleItems, state.shadowVisibleItems);
    std::swap(m_meshUploadsPending, state.meshUploadsPending);
    std::swap(m_droppedRenderItems, state.droppedRenderItems);
    std::swap(m_droppedMaterialItems, state.droppedMaterialItems);
    std::swap(m_droppedMaterialOverrides, state.droppedMaterialOverrides);
}

MeshPass::SceneRenderState& MeshPass::secondarySceneState(RenderSceneId scene) {
    if (scene.index >= m_secondaryScenes.size()) {
        m_secondaryScenes.resize(scene.index + 1);
    }
    SceneRenderState& state = m_secondaryScenes[scene.index];
    if (state.generation != scene.generation) {
        // A new scene in a reused slot: start from nothing.
        releaseSceneOverrideMaterials(scene.index + 1);
        state = {};
        state.generation = scene.generation;
        state.shadowVisibleItems.reserve(maximumObjectsPerFrame);
    }
    return state;
}

void MeshPass::retireSecondaryScenes(std::span<const RenderSceneId> scenes) {
    for (const RenderSceneId scene : scenes) {
        if (scene.index < m_secondaryScenes.size() && m_secondaryScenes[scene.index].generation == scene.generation) {
            m_secondaryScenes[scene.index] = {};
            releaseSceneOverrideMaterials(scene.index + 1);
        }
    }
}

std::vector<RenderObjectId> MeshPass::outlineUnion(std::span<const MeshPassViewRequest> requests) {
    std::vector<RenderObjectId> objects;
    for (const MeshPassViewRequest& request : requests) {
        objects.insert(objects.end(), request.outlinedObjects.begin(), request.outlinedObjects.end());
    }
    if (requests.size() > 1) {
        std::ranges::sort(objects);
        const auto duplicates = std::ranges::unique(objects);
        objects.erase(duplicates.begin(), duplicates.end());
    }
    return objects;
}

bool MeshPass::buildScenePackets(const Scene& scene, const AssetManager& assets, PacketBuild& build) {
    uint64_t outlineRevision = 0;
    for (RenderObjectId outlined : m_outlinedObjects) {
        outlineRevision ^=
            static_cast<uint64_t>(outlined) + 0x9e3779b97f4a7c15ull + (outlineRevision << 6) + (outlineRevision >> 2);
    }
    const SceneRevisions& revisions = scene.revisions();
    const bool instancesMoved = m_hasCollectionInstances && (revisions.transform != m_sceneRevisions.transform ||
                                                             revisions.skin != m_sceneRevisions.skin);
    build.rebuilt =
        !m_packetCachingEnabled || m_meshUploadsPending || revisions.renderable != m_sceneRevisions.renderable ||
        revisions.material != m_sceneRevisions.material || assets.revision() != m_assetRevision ||
        outlineRevision != m_outlineRevision || m_sceneVisibilityMode != m_builtVisibilityMode || instancesMoved;
    if (build.rebuilt) {
        // Rebuilt items normally start without motion; when instances force
        // the rebuild, each record keeps last frame's transform and palette.
        std::unordered_map<uint64_t, std::pair<glm::mat4, std::vector<glm::mat4>>> previousState;
        if (instancesMoved) {
            for (RenderItem& item : m_renderItems) {
                previousState.emplace(item.recordId,
                                      std::pair(item.previousWorld, std::move(item.previousJointPalette)));
            }
        }
        m_renderItems.clear();
        const size_t previousCapacity = m_renderItems.capacity();
        if (!buildRenderItems(scene, assets, m_outlinedObjects, m_renderItems)) {
            return false;
        }
        for (RenderItem& item : m_renderItems) {
            if (const auto previous = previousState.find(item.recordId); previous != previousState.end()) {
                item.previousWorld = previous->second.first;
                item.previousJointPalette = std::move(previous->second.second);
            }
        }
        build.scannedRenderables = static_cast<uint32_t>(scene.registry().view<const MeshRendererComponent>().size());
        build.updatedObjectRecords = static_cast<uint32_t>(m_renderItems.size());
        build.allocations = m_renderItems.capacity() != previousCapacity ? 1u : 0u;
    } else if (revisions.transform != m_sceneRevisions.transform || revisions.skin != m_sceneRevisions.skin) {
        build.updatedObjectRecords = updateRenderItems(scene, scene.changedTransforms());
        if (revisions.skin != m_sceneRevisions.skin) {
            const auto skins = scene.registry().view<const SkinComponent>();
            for (Entity entity : skins) {
                build.updatedObjectRecords += updateRenderItems(scene, std::span<const Entity>(&entity, 1));
            }
        }
    }
    m_sceneRevisions = revisions;
    m_assetRevision = assets.revision();
    m_outlineRevision = outlineRevision;
    if (build.rebuilt) {
        reportRenderItemOverflow();
    }
    // buildRenderItems admits at most maximumObjectsPerFrame items, so the
    // per-frame object regions cannot overflow.
    if (m_renderItems.size() > maximumObjectsPerFrame) {
        m_log.error("Render items exceed the per-frame render capacity");
        return false;
    }
    return true;
}

uint32_t MeshPass::writeSkinPalettes(uint32_t frameIndex, uint32_t firstMatrix, uint32_t& dropped) {
    auto* skinMatrices = static_cast<glm::mat4*>(m_cameras[frameIndex].skinPalettesMapped);
    // Last frame's palettes at the same offsets, so skinned motion vectors see
    // joint animation as well as object motion.
    auto* previousSkinMatrices = static_cast<glm::mat4*>(m_cameras[frameIndex].previousSkinPalettesMapped);
    uint32_t skinMatrixCount = firstMatrix;
    for (RenderItem& item : m_renderItems) {
        item.skinPaletteOffset = 0;
        item.skinJointCount = 0;
        if (item.jointPalette.empty()) {
            continue;
        }
        if (skinMatrixCount + item.jointPalette.size() > maximumSkinMatricesPerFrame) {
            // Out of palette space: the item draws unskinned (in its bind
            // pose) this frame.
            ++dropped;
            continue;
        }
        item.skinPaletteOffset = skinMatrixCount;
        item.skinJointCount = static_cast<uint32_t>(item.jointPalette.size());
        std::copy(item.jointPalette.begin(), item.jointPalette.end(), skinMatrices + skinMatrixCount);
        const std::vector<glm::mat4>& previousPalette = item.previousJointPalette.size() == item.jointPalette.size()
                                                            ? item.previousJointPalette
                                                            : item.jointPalette;
        std::copy(previousPalette.begin(), previousPalette.end(), previousSkinMatrices + skinMatrixCount);
        skinMatrixCount += static_cast<uint32_t>(item.jointPalette.size());
    }
    if (skinMatrixCount > firstMatrix) {
        const VkDeviceSize offset = sizeof(glm::mat4) * firstMatrix;
        const VkDeviceSize size = sizeof(glm::mat4) * (skinMatrixCount - firstMatrix);
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].skinPalettes.allocation, offset, size);
        vmaFlushAllocation(m_allocator, m_cameras[frameIndex].previousSkinPalettes.allocation, offset, size);
    }
    return skinMatrixCount;
}

bool MeshPass::buildSecondaryScene(const AssetManager& assets, std::span<const MeshPassViewRequest> views) {
    const MeshPassViewRequest& first = views.front();
    SceneRenderState& state = secondarySceneState(first.sceneId);
    exchangeSceneState(state);
    m_sceneKey = first.sceneId.index + 1;
    const SceneVisibilityMode frameVisibilityMode = m_sceneVisibilityMode;
    // A secondary scene is an editor's document: it honours the editor's
    // viewport hide even while the frame's scene plays.
    m_sceneVisibilityMode = SceneVisibilityMode::Editor;
    m_outlinedObjects = outlineUnion(views);
    if (!state.built) {
        // Revision counters start alike in every scene; never trust a match.
        m_sceneRevisions = {};
    }
    PacketBuild build;
    const bool built = buildScenePackets(*first.scene, assets, build);
    m_sceneVisibilityMode = frameVisibilityMode;
    m_sceneKey = 0;
    exchangeSceneState(state);
    state.built = state.built || built;
    return built;
}

bool MeshPass::recordSecondaryScene(VkCommandBuffer commandBuffer, uint32_t frameIndex, const AssetManager& assets,
                                    RenderRuntime& rendering, std::span<const MeshPassViewRequest> views,
                                    FrameTaskExecutor& frameTasks, double previousGpuFrameMilliseconds,
                                    const FrameState& frame, uint32_t& skinMatrixCount) {
    const MeshPassViewRequest& first = views.front();
    SceneRenderState& state = secondarySceneState(first.sceneId);
    // Preview lighting: the frame scene's lights, shadows, environment maps,
    // probes, irradiance volume, and fog stay out of these views.
    FrameState preview;
    preview.base = frame.base;
    preview.base.shadowSettings.y = 0.0f;
    preview.base.irradianceVolumeCounts = glm::uvec4(0u);
    preview.cascadeCount = frame.cascadeCount;
    preview.environmentState.specularMaximumLod = frame.environmentState.specularMaximumLod;
    preview.previewScene = true;
    exchangeSceneState(state);
    m_sceneKey = first.sceneId.index + 1;
    const SceneVisibilityMode frameVisibilityMode = m_sceneVisibilityMode;
    m_sceneVisibilityMode = SceneVisibilityMode::Editor;
    uint32_t dropped = 0;
    skinMatrixCount = writeSkinPalettes(frameIndex, skinMatrixCount, dropped);
    bool recorded = true;
    OptionalPassTimestamps noTimestamps(commandBuffer, VK_NULL_HANDLE);
    for (const MeshPassViewRequest& request : views) {
        const ViewFrame viewFrame =
            prepareViewFrame(*m_views[request.view], request, *request.scene, previousGpuFrameMilliseconds);
        if (!recordView(commandBuffer, frameIndex, *request.scene, assets, rendering, request, viewFrame, false,
                        frameTasks, VK_NULL_HANDLE, noTimestamps, preview)) {
            recorded = false;
            break;
        }
    }
    m_gpuScene.retireDeltas();
    for (RenderItem& item : m_renderItems) {
        item.previousWorld = item.world;
        item.previousJointPalette = item.jointPalette;
    }
    m_sceneVisibilityMode = frameVisibilityMode;
    m_sceneKey = 0;
    exchangeSceneState(state);
    return recorded;
}
