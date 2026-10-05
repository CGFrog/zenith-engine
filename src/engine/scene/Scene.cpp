#include "Scene.hpp"

#include "../audio/AudioSystem.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>
#include <limits>

Scene::Scene() {
    m_changedTransforms.reserve(256);
    m_dirtyTransformRoots.reserve(256);
    m_physicsLifecycleEvents.reserve(256);
}

Entity Scene::createEntity(std::string name) {
    Entity entity = entt::null;
    const SceneResult result = createEntityWithId({m_nextEntityId}, std::move(name), entity);
    if (!result) {
        return entt::null;
    }
    return entity;
}

SceneResult Scene::createEntityWithId(SceneEntityId id, std::string name, Entity& entity) {
    if (!id) {
        return {.error = "Scene entity ID zero is reserved"};
    }
    if (findEntity(id) != entt::null) {
        return {.error = "Duplicate scene entity ID " + std::to_string(id.value)};
    }

    const Entity createdEntity = m_registry.create();
    m_registry.emplace<SceneEntityIdComponent>(createdEntity, id);
    m_registry.emplace<NameComponent>(createdEntity, std::move(name));
    m_registry.emplace<TransformComponent>(createdEntity);
    auto& cache = m_registry.emplace<TransformCacheComponent>(createdEntity);
    m_registry.emplace<RelationshipComponent>(createdEntity);
    m_entitiesById.emplace(id.value, createdEntity);
    m_nextEntityId =
        id.value == std::numeric_limits<uint64_t>::max() ? id.value : std::max(m_nextEntityId, id.value + 1);
    entity = createdEntity;
    queueTransformRoot(createdEntity, cache);
    ++m_revisions.transform;
    ++m_renderPacketRevision;
    return {.success = true};
}

SceneEntityId Scene::reserveEntityId() {
    const SceneEntityId reserved{m_nextEntityId};
    if (m_nextEntityId != std::numeric_limits<uint64_t>::max()) {
        ++m_nextEntityId;
    }
    return reserved;
}

SceneEntityId Scene::nextEntityId() const {
    return {m_nextEntityId};
}

SceneEntityId Scene::id(Entity entity) const {
    const SceneEntityIdComponent* component = m_registry.try_get<SceneEntityIdComponent>(entity);
    return component == nullptr ? SceneEntityId{} : component->id;
}

Entity Scene::findEntity(SceneEntityId id) const {
    const auto found = m_entitiesById.find(id.value);
    if (found == m_entitiesById.end()) {
        return entt::null;
    }
    assert(m_registry.valid(found->second) && this->id(found->second) == id);
    return found->second;
}

Entity Scene::findChild(Entity parent, std::string_view name) const {
    const auto* relationship = m_registry.valid(parent) ? m_registry.try_get<RelationshipComponent>(parent) : nullptr;
    if (relationship == nullptr) {
        return entt::null;
    }
    for (const Entity child : relationship->children) {
        const auto* childName = m_registry.try_get<NameComponent>(child);
        if (childName != nullptr && childName->name == name) {
            return child;
        }
    }
    return entt::null;
}

Entity Scene::findDescendant(Entity root, std::string_view name) const {
    const auto* relationship = m_registry.valid(root) ? m_registry.try_get<RelationshipComponent>(root) : nullptr;
    if (relationship == nullptr) {
        return entt::null;
    }
    std::vector<Entity> pending(relationship->children.rbegin(), relationship->children.rend());
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        if (const auto* entityName = m_registry.try_get<NameComponent>(entity);
            entityName != nullptr && entityName->name == name) {
            return entity;
        }
        if (const auto* children = m_registry.try_get<RelationshipComponent>(entity)) {
            pending.insert(pending.end(), children->children.rbegin(), children->children.rend());
        }
    }
    return entt::null;
}

void Scene::setParent(Entity child, Entity parent) {
    reparent(child, parent);
}

SceneResult Scene::reparent(Entity child, Entity parent, ReparentMode mode) {
    if (!m_registry.valid(child)) {
        return {.error = "Cannot reparent an invalid child entity"};
    }
    if (parent != entt::null && !m_registry.valid(parent)) {
        return {.error = "Cannot reparent to an invalid parent entity"};
    }
    if (child == parent) {
        return {.error = "An entity cannot be its own parent"};
    }

    for (Entity ancestor = parent; ancestor != entt::null && m_registry.valid(ancestor);) {
        if (ancestor == child) {
            return {.error = "Reparenting would create a hierarchy cycle"};
        }

        ancestor = m_registry.get_or_emplace<RelationshipComponent>(ancestor).parent;
    }

    auto& relationship = m_registry.get_or_emplace<RelationshipComponent>(child);
    if (relationship.parent == parent) {
        return {.success = true};
    }

    glm::mat4 preservedWorld(1.0f);
    glm::mat4 parentWorld(1.0f);
    if (mode == ReparentMode::PreserveWorldTransform) {
        updateWorldTransforms();
        preservedWorld = m_registry.get<TransformComponent>(child).world;
        if (parent != entt::null) {
            parentWorld = m_registry.get<TransformComponent>(parent).world;
            if (std::abs(glm::determinant(parentWorld)) < 1.0e-8f) {
                return {.error = "Cannot preserve world transform under a singular parent transform"};
            }
        }
    }

    if (relationship.parent != entt::null && m_registry.valid(relationship.parent)) {
        auto& siblings = m_registry.get<RelationshipComponent>(relationship.parent).children;
        std::erase(siblings, child);
    }

    relationship.parent = parent;

    if (parent != entt::null && m_registry.valid(parent)) {
        // A new child goes before the first sibling with a larger stable id,
        // so siblings stay in id order unless setSiblingIndex moved them.
        auto& children = m_registry.get_or_emplace<RelationshipComponent>(parent).children;
        if (std::find(children.begin(), children.end(), child) == children.end()) {
            const uint64_t childId = id(child).value;
            const auto position = std::find_if(children.begin(), children.end(),
                                               [this, childId](Entity sibling) { return id(sibling).value > childId; });
            children.insert(position, child);
        }
    }

    if (mode == ReparentMode::PreserveWorldTransform) {
        m_registry.get<TransformComponent>(child).local = glm::inverse(parentWorld) * preservedWorld;
    }
    markTransformDirty(child);
    updateWorldTransforms();
    return {.success = true};
}

SceneResult Scene::setSiblingIndex(Entity child, size_t index) {
    if (!m_registry.valid(child)) {
        return {.error = "Cannot order an invalid entity"};
    }
    const Entity parent = m_registry.get<RelationshipComponent>(child).parent;
    if (parent == entt::null || !m_registry.valid(parent)) {
        return {.error = "Only children have a sibling order; root entities are listed by id"};
    }
    auto& siblings = m_registry.get<RelationshipComponent>(parent).children;
    std::erase(siblings, child);
    const auto position = siblings.begin() + static_cast<std::ptrdiff_t>(std::min(index, siblings.size()));
    siblings.insert(position, child);
    return {.success = true};
}

SceneResult Scene::duplicateSubtree(Entity source, Entity parent, Entity& duplicate) {
    if (!m_registry.valid(source)) {
        return {.error = "Cannot duplicate an invalid entity"};
    }
    if (parent != entt::null && !m_registry.valid(parent)) {
        return {.error = "Cannot duplicate beneath an invalid parent"};
    }
    duplicate = cloneBranch(source, parent, true);
    if (duplicate == entt::null) {
        return {.error = "Could not duplicate entity subtree"};
    }
    const SceneEntityId sourceRoot = id(source);
    const SceneEntityId duplicateRoot = id(duplicate);
    std::vector<Entity> pending{duplicate};
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        if (auto* node = m_registry.try_get<ModelNodeComponent>(entity); node && node->instanceRoot == sourceRoot) {
            node->instanceRoot = duplicateRoot;
        }
        if (auto* skin = m_registry.try_get<SkinComponent>(entity); skin && skin->instanceRoot == sourceRoot) {
            skin->instanceRoot = duplicateRoot;
            skin->jointPalette.clear();
        }
        const auto& children = m_registry.get<RelationshipComponent>(entity).children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    return {.success = true};
}

SceneResult Scene::instantiateModel(Entity root, ModelHandle handle, const AssetManager& assets) {
    if (!m_registry.valid(root)) {
        return {.error = "Cannot instantiate a model on an invalid entity"};
    }

    const ModelAsset* model = assets.model(handle);

    if (!model) {
        return {.error = "Cannot instantiate an invalid model handle"};
    }

    if (m_registry.any_of<ModelComponent>(root)) {
        return {.error = "Entity already owns a model instance"};
    }

    m_registry.emplace_or_replace<ModelComponent>(root, handle);
    const SceneEntityId rootId = id(root);

    std::vector<Entity> entities(model->nodes.size(), static_cast<Entity>(entt::null));

    for (size_t i = 0; i < model->nodes.size(); ++i) {
        const ModelNode& node = model->nodes[i];
        entities[i] = createEntity(node.name);
        m_registry.get<TransformComponent>(entities[i]).local = node.localTransform;
        m_registry.emplace<ModelNodeComponent>(entities[i], handle, rootId, static_cast<uint32_t>(i));
        if (!node.morphWeights.empty()) {
            m_registry.emplace<MorphWeightsComponent>(entities[i], node.morphWeights);
        }
        if (node.mesh) {
            attachMesh(entities[i], node.mesh);
        }
        if (node.skin >= 0) {
            m_registry.emplace<SkinComponent>(entities[i], handle, rootId, static_cast<uint32_t>(node.skin));
        }
    }

    for (size_t i = 0; i < model->nodes.size(); ++i) {
        const int32_t parentIndex = model->nodes[i].parent;
        setParent(entities[i], parentIndex >= 0 ? entities[static_cast<size_t>(parentIndex)] : root);
    }

    if (!model->animations.empty()) {
        m_registry.emplace<AnimationComponent>(root);
    }

    return {.success = true};
}

MeshRendererComponent& Scene::attachMesh(Entity entity, MeshHandle mesh) {
    ++m_revisions.renderable;
    ++m_renderPacketRevision;
    return m_registry.emplace_or_replace<MeshRendererComponent>(entity, mesh);
}

void Scene::destroyEntity(Entity entity) {
    if (!m_registry.valid(entity)) {
        return;
    }

    const auto children = m_registry.get_or_emplace<RelationshipComponent>(entity).children;

    for (Entity child : children) {
        destroyEntity(child);
    }

    if (m_registry.any_of<ColliderComponent, RigidBodyComponent>(entity)) {
        m_physicsLifecycleEvents.push_back({entity, PhysicsLifecycleOperation::Destroy});
    }
    const bool renderable = m_registry.any_of<MeshRendererComponent, CollectionInstanceComponent>(entity);
    const bool light =
        m_registry.any_of<DirectionalLightComponent, EnvironmentLightComponent, PointLightComponent, SpotLightComponent,
                          ReflectionProbeComponent, IrradianceVolumeComponent, DecalComponent>(entity);
    const bool camera = m_registry.any_of<CameraComponent>(entity);
    const bool skin = m_registry.any_of<SkinComponent, MorphWeightsComponent>(entity);
    setParent(entity, entt::null);
    if (const auto* sceneId = m_registry.try_get<SceneEntityIdComponent>(entity)) {
        m_entitiesById.erase(sceneId->id.value);
    }
    m_registry.destroy(entity);
    m_revisions.transform++;
    m_revisions.renderable += renderable ? 1 : 0;
    m_revisions.light += light ? 1 : 0;
    m_revisions.camera += camera ? 1 : 0;
    m_revisions.skin += skin ? 1 : 0;
    ++m_renderPacketRevision;
}

void Scene::markTransient(Entity entity) {
    if (m_registry.valid(entity)) {
        m_registry.emplace_or_replace<TransientComponent>(entity);
    }
}

bool Scene::isTransient(Entity entity) const {
    if (m_registry.view<const TransientComponent>().empty()) {
        return false;
    }
    for (Entity current = entity; m_registry.valid(current);) {
        if (m_registry.all_of<TransientComponent>(current)) {
            return true;
        }
        const auto* relationship = m_registry.try_get<RelationshipComponent>(current);
        current = relationship != nullptr ? relationship->parent : Entity{entt::null};
    }
    return false;
}

std::vector<Entity> Scene::transientEntities() const {
    std::vector<Entity> entities;
    const auto tagged = m_registry.view<const TransientComponent>();
    for (const Entity root : tagged) {
        // A tagged entity under a tagged ancestor is listed with that subtree.
        const auto* relationship = m_registry.try_get<RelationshipComponent>(root);
        if (relationship != nullptr && isTransient(relationship->parent)) {
            continue;
        }
        const size_t first = entities.size();
        entities.push_back(root);
        for (size_t index = first; index < entities.size(); ++index) {
            if (const auto* node = m_registry.try_get<RelationshipComponent>(entities[index])) {
                entities.insert(entities.end(), node->children.begin(), node->children.end());
            }
        }
    }
    return entities;
}

size_t Scene::destroyTransient() {
    const std::vector<Entity> entities = transientEntities();
    // Destroying a subtree root destroys its descendants with it.
    for (const Entity entity : entities) {
        if (m_registry.valid(entity) && m_registry.all_of<TransientComponent>(entity)) {
            destroyEntity(entity);
        }
    }
    return entities.size();
}

void Scene::clear() {
    m_registry.clear();
    m_nextEntityId = 1;
    m_changedTransforms.clear();
    m_dirtyTransformRoots.clear();
    m_entitiesById.clear();
    m_physicsLifecycleEvents.clear();
    m_collections = {};
    ++m_revisions.transform;
    ++m_revisions.renderable;
    ++m_revisions.light;
    ++m_revisions.camera;
    ++m_revisions.skin;
    ++m_revisions.material;
    ++m_renderPacketRevision;
    ++m_contentGeneration;
}

void Scene::replaceWith(Scene&& scene) {
    const uint64_t nextRevision = std::max(m_renderPacketRevision, scene.m_renderPacketRevision) + 1;
    const SceneRevisions nextRevisions{.transform = std::max(m_revisions.transform, scene.m_revisions.transform) + 1,
                                       .renderable = std::max(m_revisions.renderable, scene.m_revisions.renderable) + 1,
                                       .light = std::max(m_revisions.light, scene.m_revisions.light) + 1,
                                       .camera = std::max(m_revisions.camera, scene.m_revisions.camera) + 1,
                                       .skin = std::max(m_revisions.skin, scene.m_revisions.skin) + 1,
                                       .material = std::max(m_revisions.material, scene.m_revisions.material) + 1};
    m_registry = std::move(scene.m_registry);
    m_nextEntityId = scene.m_nextEntityId;
    m_lastTransformUpdateStats = scene.m_lastTransformUpdateStats;
    m_revisions = nextRevisions;
    m_changedTransforms = std::move(scene.m_changedTransforms);
    m_dirtyTransformRoots = std::move(scene.m_dirtyTransformRoots);
    m_transformUpdate = scene.m_transformUpdate;
    m_entitiesById = std::move(scene.m_entitiesById);
    m_transformAuditCountdown = scene.m_transformAuditCountdown;
    m_physicsLifecycleEvents = std::move(scene.m_physicsLifecycleEvents);
    m_renderPacketRevision = nextRevision;
    m_contentGeneration = std::max(m_contentGeneration, scene.m_contentGeneration) + 1;
    m_collections = std::move(scene.m_collections);
    scene.m_collections = {};
    scene.m_nextEntityId = 1;
    scene.m_dirtyTransformRoots.clear();
    scene.m_entitiesById.clear();
}

void Scene::swap(Scene& other) noexcept {
    m_registry.swap(other.m_registry);
    std::swap(m_nextEntityId, other.m_nextEntityId);
    std::swap(m_lastTransformUpdateStats, other.m_lastTransformUpdateStats);
    const SceneRevisions nextRevisions{.transform = std::max(m_revisions.transform, other.m_revisions.transform) + 1,
                                       .renderable = std::max(m_revisions.renderable, other.m_revisions.renderable) + 1,
                                       .light = std::max(m_revisions.light, other.m_revisions.light) + 1,
                                       .camera = std::max(m_revisions.camera, other.m_revisions.camera) + 1,
                                       .skin = std::max(m_revisions.skin, other.m_revisions.skin) + 1,
                                       .material = std::max(m_revisions.material, other.m_revisions.material) + 1};
    m_revisions = nextRevisions;
    other.m_revisions = {.transform = nextRevisions.transform + 1,
                         .renderable = nextRevisions.renderable + 1,
                         .light = nextRevisions.light + 1,
                         .camera = nextRevisions.camera + 1,
                         .skin = nextRevisions.skin + 1,
                         .material = nextRevisions.material + 1};
    std::swap(m_changedTransforms, other.m_changedTransforms);
    std::swap(m_dirtyTransformRoots, other.m_dirtyTransformRoots);
    std::swap(m_transformUpdate, other.m_transformUpdate);
    std::swap(m_entitiesById, other.m_entitiesById);
    std::swap(m_transformAuditCountdown, other.m_transformAuditCountdown);
    std::swap(m_physicsLifecycleEvents, other.m_physicsLifecycleEvents);
    std::swap(m_collections, other.m_collections);
    const uint64_t nextRevision = std::max(m_renderPacketRevision, other.m_renderPacketRevision) + 1;
    m_renderPacketRevision = nextRevision;
    other.m_renderPacketRevision = nextRevision + 1;
    const uint64_t nextGeneration = std::max(m_contentGeneration, other.m_contentGeneration) + 1;
    m_contentGeneration = nextGeneration;
    other.m_contentGeneration = nextGeneration + 1;
}

void Scene::updateBranch(Entity entity, const glm::mat4& parentWorld, bool parentChanged) {
    ++m_lastTransformUpdateStats.visited;
    auto& transform = m_registry.get_or_emplace<TransformComponent>(entity);
    auto& cache = m_registry.get_or_emplace<TransformCacheComponent>(entity);
    const Entity parent = m_registry.get_or_emplace<RelationshipComponent>(entity).parent;
    const bool localChanged = !cache.valid || cache.local != transform.local || cache.parent != parent;
    const bool changed = parentChanged || cache.dirty || localChanged;
    if (changed) {
        if (!m_collectingTransformChanges) {
            m_changedTransforms.clear();
            m_collectingTransformChanges = true;
        }
        transform.world = parentWorld * transform.local;
        cache.local = transform.local;
        cache.parent = parent;
        cache.valid = true;
        cache.dirty = false;
        ++m_lastTransformUpdateStats.recomputed;
        m_changedTransforms.push_back(entity);
    }
    // Copied: recursing may emplace components and move this one's storage.
    const glm::mat4 world = transform.world;
    // By index, re-reading the children each time, so the walk needs no copy
    // of the list and stays valid if the list changes underneath it.
    for (size_t index = 0;; ++index) {
        const auto* relationship = m_registry.try_get<RelationshipComponent>(entity);
        if (relationship == nullptr || index >= relationship->children.size()) {
            break;
        }
        const Entity child = relationship->children[index];
        if (m_registry.valid(child)) {
            updateBranch(child, world, changed);
        }
    }
}

void Scene::updateWorldTransforms() {
    m_lastTransformUpdateStats = {};
    m_collectingTransformChanges = false;
    updateDirtyTransformRoots();
    // The audit runs after the marked branches are updated, so whatever it
    // still finds out of date was changed without a mark.
    bool audit = false;
    if (m_transformAuditInterval != 0) {
        audit = m_transformAuditCountdown == 0;
        m_transformAuditCountdown = audit ? m_transformAuditInterval - 1 : m_transformAuditCountdown - 1;
    }
    if (audit && auditUnmarkedTransforms() > 0) {
        updateDirtyTransformRoots();
    }
    if (m_lastTransformUpdateStats.recomputed > 0) {
        ++m_revisions.transform;
        ++m_renderPacketRevision;
    }
}

void Scene::updateDirtyTransformRoots() {
    // Zero is every cache's initial rootUpdate, so it is never used.
    m_transformUpdate = m_transformUpdate == std::numeric_limits<uint32_t>::max() ? 1 : m_transformUpdate + 1;
    for (Entity entity : m_dirtyTransformRoots) {
        if (!m_registry.valid(entity)) {
            continue;
        }
        // The topmost queued ancestor updates the whole branch.
        Entity root = entity;
        for (Entity parent = m_registry.get_or_emplace<RelationshipComponent>(root).parent;
             parent != entt::null && m_registry.valid(parent);
             parent = m_registry.get_or_emplace<RelationshipComponent>(root).parent) {
            const auto* parentCache = m_registry.try_get<TransformCacheComponent>(parent);
            if (parentCache == nullptr || !parentCache->queued) {
                break;
            }
            root = parent;
        }
        auto& rootCache = m_registry.get_or_emplace<TransformCacheComponent>(root);
        if (rootCache.rootUpdate == m_transformUpdate) {
            continue;
        }
        rootCache.rootUpdate = m_transformUpdate;
        const Entity parent = m_registry.get_or_emplace<RelationshipComponent>(root).parent;
        const glm::mat4 parentWorld = parent != entt::null && m_registry.valid(parent)
                                          ? m_registry.get<TransformComponent>(parent).world
                                          : glm::mat4(1.0f);
        updateBranch(root, parentWorld, false);
    }
    for (Entity entity : m_dirtyTransformRoots) {
        if (auto* cache = m_registry.try_get<TransformCacheComponent>(entity)) {
            cache->queued = false;
        }
    }
    m_dirtyTransformRoots.clear();
}

uint32_t Scene::auditUnmarkedTransforms() {
    uint32_t unmarked = 0;
    const auto view = m_registry.view<const TransformComponent, const RelationshipComponent>();
    for (Entity entity : view) {
        const auto* cache = m_registry.try_get<TransformCacheComponent>(entity);
        const auto& transform = view.get<const TransformComponent>(entity);
        const Entity parent = view.get<const RelationshipComponent>(entity).parent;
        if (cache != nullptr && cache->valid && !cache->dirty && cache->local == transform.local &&
            cache->parent == parent) {
            continue;
        }
        ++unmarked;
        markTransformDirty(entity);
        auto& marked = m_registry.get<TransformCacheComponent>(entity);
        if (!marked.unmarkedReported) {
            marked.unmarkedReported = true;
            const auto* name = m_registry.try_get<NameComponent>(entity);
            m_log.warning("Entity " + std::to_string(id(entity).value) + " '" +
                          (name != nullptr ? name->name : std::string()) +
                          "' changed its transform without Scene::markTransformDirty, so it updated late. Use "
                          "Scene::setLocalTransform or another transform setter, or mark the entity after writing "
                          "TransformComponent::local.");
        }
    }
    m_lastTransformUpdateStats.unmarked = unmarked;
    return unmarked;
}

void Scene::markTransformDirty(Entity entity) {
    if (m_registry.valid(entity)) {
        auto& cache = m_registry.get_or_emplace<TransformCacheComponent>(entity);
        cache.dirty = true;
        queueTransformRoot(entity, cache);
    }
}

void Scene::queueTransformRoot(Entity entity, TransformCacheComponent& cache) {
    if (!cache.queued) {
        cache.queued = true;
        m_dirtyTransformRoots.push_back(entity);
    }
}

void Scene::markRenderableDirty(Entity entity) {
    if (m_registry.valid(entity)) {
        ++m_revisions.renderable;
        ++m_renderPacketRevision;
    }
}

void Scene::markLightDirty(Entity entity) {
    if (m_registry.valid(entity)) {
        ++m_revisions.light;
    }
}

void Scene::markCameraDirty(Entity entity) {
    if (m_registry.valid(entity)) {
        ++m_revisions.camera;
    }
}

void Scene::markSkinDirty(Entity entity) {
    if (m_registry.valid(entity)) {
        ++m_revisions.skin;
        ++m_renderPacketRevision;
    }
}

void Scene::markMaterialDirty(Entity entity) {
    if (m_registry.valid(entity)) {
        ++m_revisions.material;
        ++m_renderPacketRevision;
    }
}

void Scene::markPhysicsDirty(Entity entity) {
    if (m_registry.valid(entity)) {
        m_physicsLifecycleEvents.push_back({entity, PhysicsLifecycleOperation::Upsert});
    }
}

const SceneTransformUpdateStats& Scene::lastTransformUpdateStats() const {
    return m_lastTransformUpdateStats;
}

const SceneRevisions& Scene::revisions() const {
    return m_revisions;
}

std::span<const Entity> Scene::changedTransforms() const {
    return m_changedTransforms;
}

std::span<const PhysicsLifecycleEvent> Scene::physicsLifecycleEvents() const {
    return m_physicsLifecycleEvents;
}

void Scene::retirePhysicsLifecycleEvents() {
    m_physicsLifecycleEvents.clear();
}

uint64_t Scene::renderPacketRevision() const {
    return m_renderPacketRevision;
}

uint64_t Scene::contentGeneration() const {
    return m_contentGeneration;
}

entt::registry& Scene::registry() {
    return m_registry;
}

const entt::registry& Scene::registry() const {
    return m_registry;
}

const SceneCollections& Scene::collections() const {
    return m_collections;
}

void Scene::setCollections(SceneCollections collections) {
    m_collections = std::move(collections);
    ++m_revisions.renderable;
    ++m_revisions.light;
    ++m_renderPacketRevision;
}

Entity Scene::cloneBranch(Entity source, Entity parent, bool appendCopySuffix) {
    const auto& sourceName = m_registry.get<NameComponent>(source).name;
    const Entity duplicate = createEntity(sourceName + (appendCopySuffix ? " Copy" : ""));
    m_registry.get<TransformComponent>(duplicate).local = m_registry.get<TransformComponent>(source).local;

    const auto copy = [this, source, duplicate]<typename Component>() {
        if (const Component* component = m_registry.try_get<Component>(source)) {
            m_registry.emplace<Component>(duplicate, *component);
        }
    };
    copy.operator()<ModelComponent>();
    copy.operator()<ModelNodeComponent>();
    copy.operator()<AnimationComponent>();
    copy.operator()<SkinComponent>();
    copy.operator()<MorphWeightsComponent>();
    copy.operator()<MeshRendererComponent>();
    copy.operator()<MaterialOverrideComponent>();
    copy.operator()<CameraComponent>();
    copy.operator()<DirectionalLightComponent>();
    copy.operator()<EnvironmentLightComponent>();
    copy.operator()<VolumetricFogComponent>();
    copy.operator()<ParticleEmitterComponent>();
    copy.operator()<PointLightComponent>();
    copy.operator()<SpotLightComponent>();
    copy.operator()<ReflectionProbeComponent>();
    copy.operator()<IrradianceVolumeComponent>();
    copy.operator()<DecalComponent>();
    copy.operator()<ColliderComponent>();
    copy.operator()<RigidBodyComponent>();
    copy.operator()<AudioListenerComponent>();
    copy.operator()<AudioEmitterComponent>();
    copy.operator()<PreservedComponentsComponent>();
    copy.operator()<CollectionMembershipComponent>();
    copy.operator()<CollectionInstanceComponent>();
    copy.operator()<VisibilityComponent>();
    // A tag type has no value to copy.
    if (m_registry.all_of<TransientComponent>(source)) {
        m_registry.emplace<TransientComponent>(duplicate);
    }

    reparent(duplicate, parent);
    const std::vector<Entity> children = m_registry.get<RelationshipComponent>(source).children;
    for (Entity child : children) {
        if (m_registry.valid(child)) {
            cloneBranch(child, duplicate, false);
        }
    }
    return duplicate;
}
