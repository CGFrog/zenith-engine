#pragma once

#include "../assets/Assets.hpp"
#include "../physics/PhysicsComponents.hpp"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <vector>

using Entity = entt::entity;

struct NameComponent {
    std::string name;
};

struct TransformComponent {
    glm::mat4 local{1.0f};
    glm::mat4 world{1.0f};
};

struct RelationshipComponent {
    Entity parent{entt::null};
    std::vector<Entity> children;
};

struct ModelComponent {
    ModelHandle model{};
};

struct MeshRendererComponent {
    MeshHandle mesh{};
};

struct CameraComponent {
    float verticalFovRadians{glm::radians(60.0f)};
    float nearPlane{0.1f};
    float farPlane{1000.0f};
    bool primary{true};
};

struct SceneResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

class Scene {
  public:
    Entity createEntity(std::string name = {});
    void setParent(Entity child, Entity parent);
    SceneResult instantiateModel(Entity root, ModelHandle model, const AssetManager& assets);
    MeshRendererComponent& attachMesh(Entity entity, MeshHandle mesh);
    void destroyEntity(Entity entity);
    void updateWorldTransforms();

    entt::registry& registry();
    const entt::registry& registry() const;

  private:
    void updateBranch(Entity entity, const glm::mat4& parentWorld);

    entt::registry m_registry;
};
