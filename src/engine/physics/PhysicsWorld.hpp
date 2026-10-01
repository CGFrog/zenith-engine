#pragma once

#include "../scene/Scene.hpp"
#include "PhysicsComponents.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace JPH {
class BodyInterface;
class PhysicsSystem;
} // namespace JPH

struct PhysicsLayerDefinition {
    PhysicsLayer layer{PhysicsLayer::Static};
    uint32_t categoryBits{1};
    uint32_t collidesWithBits{0xffffffffu};
    bool moving{false};
};

struct PhysicsWorldConfig {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float fixedTimeStep{1.0f / 60.0f};
    uint32_t maxSubSteps{4};
    uint32_t maxBodies{4096};
    uint32_t maxBodyPairs{16384};
    uint32_t maxContactConstraints{10240};
    uint32_t workerThreads{0};
    size_t temporaryAllocatorBytes{10 * 1024 * 1024};
    // Zero disables the reconciliation audit. It is intentionally frequent in
    // debug builds and may be raised for large benchmark scenes.
    uint32_t debugAuditInterval{300};
    std::vector<PhysicsLayerDefinition> layers{
        {PhysicsLayer::Static, 1u << 0u, (1u << 1u) | (1u << 2u), false},
        {PhysicsLayer::Dynamic, 1u << 1u, 0xffffffffu, true},
        {PhysicsLayer::Player, 1u << 2u, 0xffffffffu, true},
    };
};

enum class ContactEventType {
    Begin,
    Persist,
    End,
};

struct ContactEvent {
    ContactEventType type{ContactEventType::Begin};
    Entity first{entt::null};
    Entity second{entt::null};
    glm::vec3 normal{0.0f};
};

struct PhysicsRayHit {
    Entity entity{entt::null};
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    float fraction{0.0f};
};

struct PhysicsResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

struct PhysicsStats {
    double transformMilliseconds{0.0};
    double synchronizeInMilliseconds{0.0};
    double simulationMilliseconds{0.0};
    double synchronizeOutMilliseconds{0.0};
    uint32_t bodies{0};
    uint32_t activeBodies{0};
    uint32_t sleepingBodies{0};
    uint32_t contactEvents{0};
    uint32_t lifecycleOperations{0};
    uint32_t auditedEntities{0};
    uint32_t transformsReadBack{0};
};

class PhysicsWorld {
  public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    PhysicsResult init(const PhysicsWorldConfig& config = {});
    void clear();
    void shutdown();
    PhysicsResult step(Scene& scene, const AssetManager& assets, float deltaSeconds);
    void interpolate(Scene& scene, float alpha) const;
    PhysicsResult refreshBody(Entity entity, Scene& scene, const AssetManager& assets);

    bool applyForce(Entity entity, const glm::vec3& force);
    bool applyImpulse(Entity entity, const glm::vec3& impulse);
    bool setLinearVelocity(Entity entity, const glm::vec3& velocity);
    glm::vec3 linearVelocity(Entity entity) const;
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float distance, PhysicsRayHit& hit) const;

    std::span<const ContactEvent> contactEvents() const;
    const PhysicsWorldConfig& config() const;

    const PhysicsStats& stats() const {
        return m_stats;
    }

    JPH::PhysicsSystem* nativeSystem();
    JPH::BodyInterface* nativeBodyInterface();

  private:
    PhysicsResult refreshBodyFromWorld(Entity entity, Scene& scene, const AssetManager& assets);

    class Impl;
    std::unique_ptr<Impl> m_impl;
    PhysicsStats m_stats;
};
