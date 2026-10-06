// MeshPass: building render items from the scene, LOD selection, and overflow reporting.

#include "MeshPass.hpp"
#include "MeshPassInternal.hpp"

#include "engine/graphics/MaterialRecord.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

using namespace mesh_pass_detail;

bool MeshPass::appendRenderItems(const Scene& scene, const AssetManager& assets,
                                 const std::vector<RenderObjectId>& outlinedObjects, Entity entity, Entity instancer,
                                 const glm::mat4& world, RenderItemBudget& budget, std::vector<RenderItem>& items) {
    const Entity renderObject = instancer == entt::null ? entity : instancer;
    const RenderObjectId objectId = static_cast<RenderObjectId>(entt::to_integral(renderObject));
    const bool outlined = std::binary_search(outlinedObjects.begin(), outlinedObjects.end(), objectId);
    const MeshHandle meshHandle = scene.registry().get<MeshRendererComponent>(entity).mesh;
    const MeshAsset* mesh = assets.mesh(meshHandle);
    if (!mesh) {
        return false;
    }
    // A mesh whose upload could not be staged this frame (the staging ring is
    // full while a scene loads) is skipped and retried on the next frame;
    // the pending flag forces that rebuild even when nothing else changed.
    const GpuMesh* gpuMesh = ensureMesh(meshHandle, assets);
    if (!gpuMesh) {
        m_meshUploadsPending = m_meshUploadsPending ||
                               m_deferredUploads.deferredThisFrame(DeferredUploadKind::Mesh, handleKey(meshHandle));
        return true;
    }
    if (!budget.admit(static_cast<uint32_t>(mesh->primitives.size()))) {
        return true;
    }
    const SkinComponent* skin = scene.registry().try_get<SkinComponent>(entity);
    const MaterialOverrideComponent* materialOverride = scene.registry().try_get<MaterialOverrideComponent>(entity);
    // A replacement material that is not live is ignored, like a missing one.
    const bool replacesMaterial = materialOverride != nullptr && assets.material(materialOverride->material) != nullptr;
    const bool changesFactors = materialOverride != nullptr && materialOverrideChangesFactors(*materialOverride);
    const glm::vec3 center = glm::vec3(world * glm::vec4(mesh->bounds.center, 1.0f));
    const float scale = std::max(
        {glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))});
    for (const MeshPrimitive& primitive : mesh->primitives) {
        const MaterialHandle materialHandle = replacesMaterial ? materialOverride->material : primitive.material;
        const MaterialAsset* material = assets.material(materialHandle);
        if (!material) {
            return false;
        }
        // Every material slot is taken: the primitive is left out until one frees.
        const GpuMaterial* gpuMaterial = ensureMaterial(materialHandle, assets);
        if (!gpuMaterial) {
            ++m_droppedMaterialItems;
            continue;
        }
        if (changesFactors) {
            // Without a free override record the plain material is drawn.
            if (const GpuMaterial* overridden = ensureOverrideMaterial(materialHandle, *materialOverride)) {
                gpuMaterial = overridden;
            } else {
                ++m_droppedMaterialOverrides;
            }
        }
        items.push_back({.meshHandle = meshHandle,
                         .entity = entity,
                         .instancer = instancer,
                         .materialHandle = materialHandle,
                         .mesh = mesh,
                         .primitive = &primitive,
                         .lod = primitive.lod(0),
                         .material = material,
                         .gpuMesh = gpuMesh,
                         .gpuMaterial = gpuMaterial,
                         .world = world,
                         .previousWorld = world,
                         .boundsCenter = center,
                         .boundsRadius = mesh->bounds.radius * scale,
                         .outlined = outlined,
                         .jointPalette = skin ? skin->jointPalette : std::vector<glm::mat4>{}});
    }
    return true;
}

bool MeshPass::buildRenderItems(const Scene& scene, const AssetManager& assets,
                                const std::vector<RenderObjectId>& outlinedObjects, std::vector<RenderItem>& items) {
    m_meshUploadsPending = false;
    m_droppedMaterialItems = 0;
    m_droppedMaterialOverrides = 0;
    ++m_renderItemBuild;
    RenderItemBudget budget(maximumObjectsPerFrame);
    const SceneVisibility visibility(scene, m_sceneVisibilityMode);
    m_builtVisibilityMode = m_sceneVisibilityMode;
    const auto renderables = scene.registry().view<const TransformComponent, const MeshRendererComponent>();
    for (Entity entity : renderables) {
        if (!visibility.drawn(entity)) {
            continue;
        }
        if (!appendRenderItems(scene, assets, outlinedObjects, entity, entt::null,
                               renderables.get<const TransformComponent>(entity).world, budget, items)) {
            return false;
        }
    }
    // Instance draws follow the scene's own items; their position in the
    // expansion keeps record ids distinct when one member is drawn twice.
    m_collectionInstanceDraws.clear();
    expandCollectionInstances(scene, visibility, m_collectionInstanceDraws);
    m_hasCollectionInstances = !m_collectionInstanceDraws.empty();
    for (size_t ordinal = 0; ordinal < m_collectionInstanceDraws.size(); ++ordinal) {
        const CollectionInstanceDraw& draw = m_collectionInstanceDraws[ordinal];
        if (!scene.registry().all_of<MeshRendererComponent>(draw.member)) {
            continue;
        }
        const size_t first = items.size();
        if (!appendRenderItems(scene, assets, outlinedObjects, draw.member, draw.instancer, draw.world, budget,
                               items)) {
            return false;
        }
        for (size_t index = first; index < items.size(); ++index) {
            items[index].instanceOrdinal = static_cast<uint32_t>(ordinal + 1);
        }
    }
    m_droppedRenderItems = budget.dropped();
    releaseUnusedOverrideMaterials();
    std::sort(items.begin(), items.end(), [](const RenderItem& first, const RenderItem& second) {
        if (first.material->alphaMode != second.material->alphaMode) {
            return first.material->alphaMode < second.material->alphaMode;
        }
        if (first.material->doubleSided != second.material->doubleSided) {
            return first.material->doubleSided < second.material->doubleSided;
        }
        if (handleKey(first.materialHandle) != handleKey(second.materialHandle)) {
            return handleKey(first.materialHandle) < handleKey(second.materialHandle);
        }
        if (handleKey(first.meshHandle) != handleKey(second.meshHandle)) {
            return handleKey(first.meshHandle) < handleKey(second.meshHandle);
        }
        if (first.primitive->firstIndex != second.primitive->firstIndex) {
            return first.primitive->firstIndex < second.primitive->firstIndex;
        }
        return first.outlined < second.outlined;
    });
    m_objectItems.clear();
    std::unordered_map<uint64_t, GpuSceneHandle> nextGpuRecords;
    nextGpuRecords.reserve(items.size());
    m_gpuHandleItems.clear();
    for (size_t index = 0; index < items.size(); ++index) {
        RenderItem& item = items[index];
        const RenderObjectId objectId = static_cast<RenderObjectId>(entt::to_integral(item.renderObject()));
        m_objectItems[objectId].push_back(index);
        item.recordId = static_cast<uint64_t>(objectId) * 0x9e3779b97f4a7c15ull;
        item.recordId ^=
            handleKey(item.meshHandle) + 0x9e3779b97f4a7c15ull + (item.recordId << 6) + (item.recordId >> 2);
        item.recordId ^=
            handleKey(item.materialHandle) + item.primitive->firstIndex + (item.recordId << 6) + (item.recordId >> 2);
        if (item.instanceOrdinal != 0) {
            item.recordId ^= (static_cast<uint64_t>(entt::to_integral(item.entity)) << 32 | item.instanceOrdinal) +
                             0x9e3779b97f4a7c15ull + (item.recordId << 6) + (item.recordId >> 2);
        }
        const GpuSceneObject gpuObject{.recordId = item.recordId,
                                       .objectId = objectId,
                                       .meshKey = handleKey(item.meshHandle),
                                       .materialKey = handleKey(item.materialHandle),
                                       .world = item.world,
                                       .localBoundsCenter = item.mesh->bounds.center,
                                       .localBoundsRadius = item.mesh->bounds.radius,
                                       .indexCount = item.primitive->indexCount,
                                       .firstIndex = item.primitive->firstIndex};
        item.gpuSceneHandle = m_gpuScene.upsert(gpuObject);
        nextGpuRecords.emplace(item.recordId, item.gpuSceneHandle);
        m_gpuHandleItems.emplace(handleKey(item.gpuSceneHandle), index);
    }
    for (const auto& [recordId, handle] : m_gpuRecords) {
        if (!nextGpuRecords.contains(recordId)) {
            m_gpuScene.remove(handle);
        }
    }
    m_gpuRecords.swap(nextGpuRecords);
    return true;
}

void MeshPass::reportRenderItemOverflow() {
    if (m_renderItemOverflow.update(m_droppedRenderItems > 0)) {
        m_log.warning("Render items exceed the per-frame capacity of " + std::to_string(maximumObjectsPerFrame) + "; " +
                      std::to_string(m_droppedRenderItems) + " items are not drawn (the last objects in build order)");
    }
    if (m_materialOverflow.update(m_droppedMaterialItems > 0)) {
        m_log.warning("Materials exceed the renderer capacity of " + std::to_string(maximumMaterials) + "; " +
                      std::to_string(m_droppedMaterialItems) + " render items without a material slot are not drawn");
    }
    if (m_materialOverrideOverflow.update(m_droppedMaterialOverrides > 0)) {
        m_log.warning("Material overrides exceed the renderer capacity of " + std::to_string(maximumMaterialOverrides) +
                      " distinct material and factor pairs; " + std::to_string(m_droppedMaterialOverrides) +
                      " render items draw their plain material");
    }
}

uint32_t MeshPass::updateRenderItems(const Scene& scene, std::span<const Entity> changedEntities) {
    uint32_t updated = 0;
    for (Entity entity : changedEntities) {
        if (!scene.registry().valid(entity)) {
            continue;
        }
        const RenderObjectId objectId = static_cast<RenderObjectId>(entt::to_integral(entity));
        const auto found = m_objectItems.find(objectId);
        const TransformComponent* transform = scene.registry().try_get<TransformComponent>(entity);
        if (found == m_objectItems.end() || transform == nullptr) {
            continue;
        }
        const SkinComponent* skin = scene.registry().try_get<SkinComponent>(entity);
        for (size_t itemIndex : found->second) {
            RenderItem& item = m_renderItems[itemIndex];
            if (item.instancer != entt::null) {
                // Instance draws are rebuilt instead (m_hasCollectionInstances).
                continue;
            }
            item.previousWorld = item.world;
            item.world = transform->world;
            item.boundsCenter = glm::vec3(item.world * glm::vec4(item.mesh->bounds.center, 1.0f));
            const float scale = std::max({glm::length(glm::vec3(item.world[0])), glm::length(glm::vec3(item.world[1])),
                                          glm::length(glm::vec3(item.world[2]))});
            item.boundsRadius = item.mesh->bounds.radius * scale;
            if (skin) {
                item.jointPalette = skin->jointPalette;
            }
            m_gpuScene.update(item.gpuSceneHandle, {.recordId = item.recordId,
                                                    .objectId = objectId,
                                                    .meshKey = handleKey(item.meshHandle),
                                                    .materialKey = handleKey(item.materialHandle),
                                                    .world = item.world,
                                                    .localBoundsCenter = item.mesh->bounds.center,
                                                    .localBoundsRadius = item.mesh->bounds.radius,
                                                    .indexCount = item.primitive->indexCount,
                                                    .firstIndex = item.primitive->firstIndex});
            ++updated;
        }
    }
    return updated;
}

void MeshPass::selectRenderItemLods(const MeshLodView& view) {
    for (RenderItem& item : m_renderItems) {
        const uint32_t level =
            selectMeshLod(*item.primitive, view, item.boundsCenter, item.boundsRadius, maximumAxisScale(item.world));
        item.lod = item.primitive->lod(level);
    }
}
