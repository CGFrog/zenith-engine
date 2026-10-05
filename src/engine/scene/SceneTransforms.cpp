// Scene's gameplay transform API: setters that mark entities dirty and
// readers that resolve the hierarchy on demand.
#include "Scene.hpp"

#include <algorithm>
#include <utility>

namespace {
const glm::mat4 identityTransform(1.0f);
} // namespace

TransformComponent* Scene::mutableTransform(Entity entity) {
    return m_registry.valid(entity) ? m_registry.try_get<TransformComponent>(entity) : nullptr;
}

void Scene::setLocalTransform(Entity entity, const glm::mat4& local) {
    if (TransformComponent* transform = mutableTransform(entity)) {
        transform->local = local;
        markTransformDirty(entity);
    }
}

void Scene::setPosition(Entity entity, const glm::vec3& position) {
    if (TransformComponent* transform = mutableTransform(entity)) {
        transform->local[3] = glm::vec4(position, 1.0f);
        markTransformDirty(entity);
    }
}

void Scene::setRotation(Entity entity, const glm::quat& rotation) {
    if (TransformComponent* transform = mutableTransform(entity)) {
        TransformTrs trs = decomposeTrs(transform->local);
        trs.rotation = rotation;
        transform->local = composeTrs(trs);
        markTransformDirty(entity);
    }
}

void Scene::setScale(Entity entity, const glm::vec3& scale) {
    if (TransformComponent* transform = mutableTransform(entity)) {
        TransformTrs trs = decomposeTrs(transform->local);
        trs.scale = scale;
        transform->local = composeTrs(trs);
        markTransformDirty(entity);
    }
}

void Scene::translate(Entity entity, const glm::vec3& offset) {
    if (TransformComponent* transform = mutableTransform(entity)) {
        transform->local[3] += glm::vec4(offset, 0.0f);
        markTransformDirty(entity);
    }
}

void Scene::setWorldTransform(Entity entity, const glm::mat4& world) {
    if (mutableTransform(entity) == nullptr) {
        return;
    }
    const auto* relationship = m_registry.try_get<RelationshipComponent>(entity);
    const Entity parent = relationship != nullptr ? relationship->parent : entt::null;
    setLocalTransform(entity, parent != entt::null && m_registry.valid(parent)
                                  ? glm::inverse(worldTransform(parent)) * world
                                  : world);
}

const glm::mat4& Scene::localTransform(Entity entity) const {
    const auto* transform = m_registry.valid(entity) ? m_registry.try_get<TransformComponent>(entity) : nullptr;
    return transform != nullptr ? transform->local : identityTransform;
}

TransformTrs Scene::localTrs(Entity entity) const {
    return decomposeTrs(localTransform(entity));
}

glm::vec3 Scene::localPosition(Entity entity) const {
    return glm::vec3(localTransform(entity)[3]);
}

glm::quat Scene::localRotation(Entity entity) const {
    return localTrs(entity).rotation;
}

glm::vec3 Scene::localScale(Entity entity) const {
    return localTrs(entity).scale;
}

glm::mat4 Scene::worldTransform(Entity entity) const {
    if (!m_registry.valid(entity)) {
        return identityTransform;
    }
    // Reads only locals, so a dirty ancestor or a pending update is already
    // accounted for. The hierarchy is acyclic (reparent rejects cycles).
    glm::mat4 world = localTransform(entity);
    const auto* relationship = m_registry.try_get<RelationshipComponent>(entity);
    while (relationship != nullptr && relationship->parent != entt::null && m_registry.valid(relationship->parent)) {
        const Entity parent = relationship->parent;
        world = localTransform(parent) * world;
        relationship = m_registry.try_get<RelationshipComponent>(parent);
    }
    return world;
}

glm::vec3 Scene::worldPosition(Entity entity) const {
    return glm::vec3(worldTransform(entity)[3]);
}

void Scene::setTransformAuditInterval(uint32_t updates) {
    m_transformAuditInterval = updates;
    m_transformAuditCountdown = std::min(m_transformAuditCountdown, updates == 0 ? 0u : updates - 1);
}

uint32_t Scene::transformAuditInterval() const {
    return m_transformAuditInterval;
}

void Scene::setLogChannel(LogChannel channel) {
    m_log = std::move(channel);
}
