#pragma once

#include "../assets/Assets.hpp"
#include "../physics/PhysicsComponents.hpp"
#include <compare>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <vector>

using Entity = entt::entity;

struct SceneEntityId {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    auto operator<=>(const SceneEntityId&) const = default;
};

struct SceneEntityIdComponent {
    SceneEntityId id;
};

struct NameComponent {
    std::string name;
};

struct TransformComponent {
    glm::mat4 local{1.0f};
    glm::mat4 world{1.0f};
};

struct SceneTransformUpdateStats {
    uint32_t visited{0};
    uint32_t recomputed{0};
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

struct DirectionalShadowSettings {
    bool enabled{true};
    // World-space half width of the orthographic shadow volume.
    float halfExtent{14.0f};
    // Distance from the camera toward its view direction used to center the shadow volume.
    float focusDistance{6.0f};
    // World-space depth covered along the light direction.
    float depthRange{60.0f};
    // Raster depth-bias factors; normalBias is a world-space receiver offset.
    float constantBias{0.5f};
    float slopeBias{1.0f};
    float normalBias{0.01f};
};

struct DirectionalLightComponent {
    // Direction from a shaded surface toward the light, in world space.
    glm::vec3 direction{0.4f, 0.8f, 0.5f};
    glm::vec3 color{1.0f, 0.91f, 0.78f};
    float intensity{5.0f};
    bool primary{true};
    DirectionalShadowSettings shadows;
};

struct EnvironmentLightComponent {
    // Linear-light colors sampled as a hemisphere around the world Y axis.
    glm::vec3 skyColor{0.20f, 0.24f, 0.34f};
    glm::vec3 groundColor{0.08f, 0.065f, 0.05f};
    float diffuseIntensity{0.6f};
    float specularIntensity{0.35f};
    bool primary{true};
};

struct SceneResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

enum class ReparentMode {
    PreserveLocalTransform,
    PreserveWorldTransform,
};

class Scene {
  public:
    Entity createEntity(std::string name = {});
    SceneResult createEntityWithId(SceneEntityId id, std::string name, Entity& entity);
    SceneEntityId id(Entity entity) const;
    Entity findEntity(SceneEntityId id) const;
    void setParent(Entity child, Entity parent);
    SceneResult reparent(Entity child, Entity parent, ReparentMode mode = ReparentMode::PreserveLocalTransform);
    SceneResult duplicateSubtree(Entity source, Entity parent, Entity& duplicate);
    SceneResult instantiateModel(Entity root, ModelHandle model, const AssetManager& assets);
    MeshRendererComponent& attachMesh(Entity entity, MeshHandle mesh);
    void destroyEntity(Entity entity);
    void clear();
    void replaceWith(Scene&& scene);
    void swap(Scene& other) noexcept;
    void updateWorldTransforms();
    void markTransformDirty(Entity entity);
    const SceneTransformUpdateStats& lastTransformUpdateStats() const;
    uint64_t renderPacketRevision() const;

    entt::registry& registry();
    const entt::registry& registry() const;

  private:
    Entity cloneBranch(Entity source, Entity parent, bool appendCopySuffix);

    struct TransformCacheComponent {
        glm::mat4 local{1.0f};
        Entity parent{entt::null};
        bool valid{false};
        bool dirty{true};
    };

    bool updateBranch(Entity entity, const glm::mat4& parentWorld, bool parentChanged);

    entt::registry m_registry;
    uint64_t m_nextEntityId{1};
    SceneTransformUpdateStats m_lastTransformUpdateStats;
    uint64_t m_renderPacketRevision{1};
};
