#include "Scene.hpp"

#include "../audio/AudioSystem.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>
#include <limits>

Scene::Scene() {
    m_changedTransforms.reserve(256);
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
    m_registry.emplace<TransformCacheComponent>(createdEntity);
    m_registry.emplace<RelationshipComponent>(createdEntity);
    m_nextEntityId =
        id.value == std::numeric_limits<uint64_t>::max() ? id.value : std::max(m_nextEntityId, id.value + 1);
    entity = createdEntity;
    ++m_revisions.transform;
    ++m_renderPacketRevision;
    return {.success = true};
}

SceneEntityId Scene::id(Entity entity) const {
    const SceneEntityIdComponent* component = m_registry.try_get<SceneEntityIdComponent>(entity);
    return component == nullptr ? SceneEntityId{} : component->id;
}

Entity Scene::findEntity(SceneEntityId id) const {
    auto view = m_registry.view<const SceneEntityIdComponent>();
    for (Entity entity : view) {
        if (view.get<const SceneEntityIdComponent>(entity).id == id) {
            return entity;
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
        auto& children = m_registry.get_or_emplace<RelationshipComponent>(parent).children;
        if (std::find(children.begin(), children.end(), child) == children.end()) {
            children.push_back(child);
        }
    }

    if (mode == ReparentMode::PreserveWorldTransform) {
        m_registry.get<TransformComponent>(child).local = glm::inverse(parentWorld) * preservedWorld;
    }
    markTransformDirty(child);
    updateWorldTransforms();
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
    const bool renderable = m_registry.any_of<MeshRendererComponent>(entity);
    const bool light = m_registry.any_of<DirectionalLightComponent, EnvironmentLightComponent, PointLightComponent,
                                         SpotLightComponent, ReflectionProbeComponent>(entity);
    const bool camera = m_registry.any_of<CameraComponent>(entity);
    const bool skin = m_registry.any_of<SkinComponent, MorphWeightsComponent>(entity);
    setParent(entity, entt::null);
    m_registry.destroy(entity);
    m_revisions.transform++;
    m_revisions.renderable += renderable ? 1 : 0;
    m_revisions.light += light ? 1 : 0;
    m_revisions.camera += camera ? 1 : 0;
    m_revisions.skin += skin ? 1 : 0;
    ++m_renderPacketRevision;
}

void Scene::clear() {
    m_registry.clear();
    m_nextEntityId = 1;
    m_changedTransforms.clear();
    m_physicsLifecycleEvents.clear();
    ++m_revisions.transform;
    ++m_revisions.renderable;
    ++m_revisions.light;
    ++m_revisions.camera;
    ++m_revisions.skin;
    ++m_revisions.material;
    ++m_renderPacketRevision;
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
    m_physicsLifecycleEvents = std::move(scene.m_physicsLifecycleEvents);
    m_renderPacketRevision = nextRevision;
    scene.m_nextEntityId = 1;
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
    std::swap(m_physicsLifecycleEvents, other.m_physicsLifecycleEvents);
    const uint64_t nextRevision = std::max(m_renderPacketRevision, other.m_renderPacketRevision) + 1;
    m_renderPacketRevision = nextRevision;
    other.m_renderPacketRevision = nextRevision + 1;
}

bool Scene::updateBranch(Entity entity, const glm::mat4& parentWorld, bool parentChanged) {
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
    const auto children = m_registry.get_or_emplace<RelationshipComponent>(entity).children;

    for (Entity child : children) {
        if (m_registry.valid(child)) {
            updateBranch(child, transform.world, changed);
        }
    }
    return changed;
}

void Scene::updateWorldTransforms() {
    m_lastTransformUpdateStats = {};
    m_collectingTransformChanges = false;
    auto view = m_registry.view<TransformComponent, RelationshipComponent>();
    for (Entity entity : view) {
        if (view.get<RelationshipComponent>(entity).parent == entt::null) {
            updateBranch(entity, glm::mat4(1.0f), false);
        }
    }
    if (m_lastTransformUpdateStats.recomputed > 0) {
        ++m_revisions.transform;
        ++m_renderPacketRevision;
    }
}

void Scene::markTransformDirty(Entity entity) {
    if (m_registry.valid(entity)) {
        m_registry.get_or_emplace<TransformCacheComponent>(entity).dirty = true;
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

entt::registry& Scene::registry() {
    return m_registry;
}

const entt::registry& Scene::registry() const {
    return m_registry;
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
    copy.operator()<CameraComponent>();
    copy.operator()<DirectionalLightComponent>();
    copy.operator()<EnvironmentLightComponent>();
    copy.operator()<PointLightComponent>();
    copy.operator()<SpotLightComponent>();
    copy.operator()<ReflectionProbeComponent>();
    copy.operator()<ColliderComponent>();
    copy.operator()<RigidBodyComponent>();
    copy.operator()<AudioListenerComponent>();
    copy.operator()<AudioEmitterComponent>();

    reparent(duplicate, parent);
    const std::vector<Entity> children = m_registry.get<RelationshipComponent>(source).children;
    for (Entity child : children) {
        if (m_registry.valid(child)) {
            cloneBranch(child, duplicate, false);
        }
    }
    return duplicate;
}
