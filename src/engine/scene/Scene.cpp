#include "Scene.hpp"

#include <algorithm>

Entity Scene::createEntity(std::string name) {
    const Entity entity = m_registry.create();
    m_registry.emplace<NameComponent>(entity, std::move(name));
    m_registry.emplace<TransformComponent>(entity);
    m_registry.emplace<RelationshipComponent>(entity);
    return entity;
}

void Scene::setParent(Entity child, Entity parent) {
    if (!m_registry.valid(child) || child == parent) {
        return;
    }

    for (Entity ancestor = parent; ancestor != entt::null && m_registry.valid(ancestor);) {
        if (ancestor == child) {
            return;
        }

        ancestor = m_registry.get_or_emplace<RelationshipComponent>(ancestor).parent;
    }

    auto& relationship = m_registry.get_or_emplace<RelationshipComponent>(child);

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

    std::vector<Entity> entities(model->nodes.size(), static_cast<Entity>(entt::null));

    for (size_t i = 0; i < model->nodes.size(); ++i) {
        const ModelNode& node = model->nodes[i];
        entities[i] = createEntity(node.name);
        m_registry.get<TransformComponent>(entities[i]).local = node.localTransform;
        if (node.mesh) {
            attachMesh(entities[i], node.mesh);
        }
    }

    for (size_t i = 0; i < model->nodes.size(); ++i) {
        const int32_t parentIndex = model->nodes[i].parent;
        setParent(entities[i], parentIndex >= 0 ? entities[static_cast<size_t>(parentIndex)] : root);
    }

    return {.success = true};
}

MeshRendererComponent& Scene::attachMesh(Entity entity, MeshHandle mesh) {
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

    setParent(entity, entt::null);
    m_registry.destroy(entity);
}

void Scene::updateBranch(Entity entity, const glm::mat4& parentWorld) {
    auto& transform = m_registry.get_or_emplace<TransformComponent>(entity);
    transform.world = parentWorld * transform.local;
    const auto children = m_registry.get_or_emplace<RelationshipComponent>(entity).children;

    for (Entity child : children) {
        if (m_registry.valid(child)) {
            updateBranch(child, transform.world);
        }
    }
}

void Scene::updateWorldTransforms() {
    auto view = m_registry.view<TransformComponent, RelationshipComponent>();
    for (Entity entity : view) {
        if (view.get<RelationshipComponent>(entity).parent == entt::null) {
            updateBranch(entity, glm::mat4(1.0f));
        }
    }
}

entt::registry& Scene::registry() {
    return m_registry;
}

const entt::registry& Scene::registry() const {
    return m_registry;
}
