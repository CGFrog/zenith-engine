#pragma once

#include "../diagnostics/Logger.hpp"
#include "../graphics/DebugDraw.hpp"
#include "../scene/Scene.hpp"
#include "CharacterController.hpp"
#include "PhysicsComponents.hpp"
#include "PhysicsLayers.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace JPH {
class BodyInterface;
class PhysicsSystem;
} // namespace JPH

struct PhysicsWorldConfig {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float fixedTimeStep{1.0f / 60.0f};
    uint32_t maxSubSteps{4};
    // Simulation capacities (docs/engine/physics.md lists their memory cost).
    // Bodies beyond maxBodies are skipped with a warning; a step that runs out
    // of body pairs or contact constraints leaves the excess contacts
    // unresolved for that step (bodies may pass through each other) and warns
    // once per episode. Each contact constraint needs
    // contactConstraintTemporaryBytes of temporaryAllocatorBytes every step,
    // which init checks.
    uint32_t maxBodies{8192};
    uint32_t maxBodyPairs{32768};
    uint32_t maxContactConstraints{16384};
    uint32_t workerThreads{0};
    size_t temporaryAllocatorBytes{16 * 1024 * 1024};
    // Zero disables the reconciliation audit. It is intentionally frequent in
    // debug builds and may be raised for large benchmark scenes.
    uint32_t debugAuditInterval{300};
    // A project's own table replaces it through PhysicsWorld::setLayers.
    PhysicsLayerTable layers{PhysicsLayerTable::defaults()};
};

// Per-step temporary memory Jolt reserves for each contact constraint
// (480 bytes of constraint plus an index, rounded up).
inline constexpr size_t contactConstraintTemporaryBytes = 512;

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

// A body entered or left a sensor (a rigid body with RigidBodyComponent::
// sensor set). Exited is also reported when either body is removed, for
// example because its entity was destroyed; the entity may then no longer
// exist, so each side also carries its scene id. Bodies are matched on whole
// bodies: a pair is inside while any of their shapes touch. A body that falls
// asleep inside stays inside (Jolt drops a sleeping body's contacts; waking
// up inside reports nothing). Two sensors never report each other.
enum class TriggerEventType {
    Entered,
    Exited,
};

struct TriggerEvent {
    TriggerEventType type{TriggerEventType::Entered};
    Entity sensor{entt::null};
    Entity other{entt::null};
    SceneEntityId sensorId;
    SceneEntityId otherId;
};

// What a query may hit: bodies on the layers in layerMask (bit i is layer i),
// except the ignored entities (for example a ray's own shooter) and, unless
// hitSensors is set, sensors.
struct PhysicsQueryFilter {
    uint32_t layerMask{~0u};
    std::span<const Entity> ignore;
    bool hitSensors{false};
};

struct PhysicsRayHit {
    Entity entity{entt::null};
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    // Along the cast, from 0 at its start to 1 at its full distance.
    float fraction{0.0f};
};

// Shapes swept or tested by queries, in world units. A capsule's axis is its
// local Y axis; halfHeight excludes the end caps.
struct PhysicsQuerySphere {
    float radius{0.5f};
};

struct PhysicsQueryCapsule {
    float halfHeight{0.5f};
    float radius{0.5f};
};

struct PhysicsQueryBox {
    glm::vec3 halfExtents{0.5f};
};

using PhysicsQueryShape = std::variant<PhysicsQuerySphere, PhysicsQueryCapsule, PhysicsQueryBox>;

// Where a query shape sits: its center and orientation.
struct PhysicsQueryPose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct PhysicsShapeHit {
    Entity entity{entt::null};
    // The contact point on the hit body, and the hit surface's normal
    // (pointing back toward the cast shape).
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    // Along the cast; zero when the shape already overlapped the body at its
    // start.
    float fraction{0.0f};
};

struct PhysicsClosestPoint {
    Entity entity{entt::null};
    // The nearest point on the body's surface, and its distance from the
    // query point (zero when the point is inside the body).
    glm::vec3 position{0.0f};
    float distance{0.0f};
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
    // Entities left without a body or character because theirs could not be
    // built (an unsupported collider, a degenerate transform, maxBodies
    // reached); each is logged once and retried when it changes.
    uint32_t skippedBodies{0};
    // Jolt ran out of body-pair, manifold, or contact-constraint capacity in
    // this step, so some contacts were not resolved.
    bool contactCapacityExceeded{false};
};

// What PhysicsWorld::drawDebug records. Shapes are collider wireframes in
// Jolt's motion-type colours (static grey, keyframed green, dynamic a colour
// per body).
struct PhysicsDebugDrawOptions {
    bool shapes{true};
    bool boundingBoxes{false};
    bool centersOfMass{false};
    bool velocities{false};
    DebugDepth depth{DebugDepth::Tested};
};

// A Jolt physics system over one Scene. Any number of worlds may be
// initialized at once (for example an editor's edit scene and its Play or
// Simulate copy, or tests) and shut down in any order: they share Jolt's
// process-wide runtime through JoltRuntimeLease, which the last world to shut
// down destroys.
class PhysicsWorld {
  public:
    PhysicsWorld();
    // logger receives warnings about skipped bodies and exceeded capacities
    // and must outlive the world; null writes them to stderr.
    explicit PhysicsWorld(Logger* logger);
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Fails on an invalid config, including an invalid layer table.
    PhysicsResult init(const PhysicsWorldConfig& config = {});
    // Replaces the collision layers, for example with the project's. Fails
    // while the world has bodies (clear it first) and for an invalid table.
    // A body whose layer the table lacks is put on Dynamic.
    PhysicsResult setLayers(const PhysicsLayerTable& layers);
    // Removes every body. Pending trigger state is dropped without Exited
    // events.
    void clear();
    void shutdown();
    // Fails only before init. An entity whose body cannot be built is left
    // out of the simulation (logged once, counted in PhysicsStats) and the
    // other bodies keep simulating; exceeded contact capacities are logged
    // once per episode.
    PhysicsResult step(Scene& scene, const AssetManager& assets, float deltaSeconds);
    void interpolate(Scene& scene, float alpha) const;
    // Rebuilds the entity's body now; unlike step, reports a body that cannot
    // be built as an error.
    PhysicsResult refreshBody(Entity entity, Scene& scene, const AssetManager& assets);

    bool applyForce(Entity entity, const glm::vec3& force);
    bool applyImpulse(Entity entity, const glm::vec3& impulse);
    bool setLinearVelocity(Entity entity, const glm::vec3& velocity);
    glm::vec3 linearVelocity(Entity entity) const;

    // Queries against the bodies as of the last step (or refresh). Each
    // returns false (leaving its output empty) without an initialized world
    // or a usable direction, distance, or shape.
    //
    // The nearest hit along direction within distance.
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float distance, PhysicsRayHit& hit,
                 const PhysicsQueryFilter& filter = {}) const;
    // Every body along the ray, nearest first, each once at its nearest hit.
    bool raycastAll(const glm::vec3& origin, const glm::vec3& direction, float distance,
                    std::vector<PhysicsRayHit>& hits, const PhysicsQueryFilter& filter = {}) const;
    // The first body shape meets when swept from start along direction.
    bool shapeCast(const PhysicsQueryShape& shape, const PhysicsQueryPose& start, const glm::vec3& direction,
                   float distance, PhysicsShapeHit& hit, const PhysicsQueryFilter& filter = {}) const;
    // Every body shape overlaps at pose, each once, in no particular order.
    bool overlap(const PhysicsQueryShape& shape, const PhysicsQueryPose& pose, std::vector<Entity>& entities,
                 const PhysicsQueryFilter& filter = {}) const;
    // The body surface nearest to point within maxDistance.
    bool closestPoint(const glm::vec3& point, float maxDistance, PhysicsClosestPoint& closest,
                      const PhysicsQueryFilter& filter = {}) const;

    std::span<const ContactEvent> contactEvents() const;
    // The sensor entries and exits of the last step, exits of removed bodies
    // first.
    std::span<const TriggerEvent> triggerEvents() const;
    // Records the bodies' debug geometry into `output` (for example
    // RenderRuntime::debugDraw()). Physics depends only on the recording
    // type, never on the renderer. Fails before init or when a debug renderer
    // that is not a PhysicsWorld's owns Jolt's one per-process slot.
    PhysicsResult drawDebug(DebugDraw& output, const PhysicsDebugDrawOptions& options = {}) const;
    // Triangle-mesh shapes held for reuse, one per (mesh, generation, scale):
    // static mesh colliders with the same mesh and scale share one. Entries
    // for meshes that are no longer live are dropped when the asset revision
    // changes; shutdown releases them all.
    size_t cachedMeshShapes() const;
    const PhysicsWorldConfig& config() const;

    const PhysicsStats& stats() const {
        return m_stats;
    }

    JPH::PhysicsSystem* nativeSystem();
    JPH::BodyInterface* nativeBodyInterface();

  private:
    PhysicsResult refreshBodyFromWorld(Entity entity, Scene& scene, const AssetManager& assets);
    // refreshBodyFromWorld for step: a body that cannot be built is skipped
    // and reported once instead of failing the step.
    void refreshOrSkipBody(Entity entity, Scene& scene, const AssetManager& assets);
    // Builds bodies skipped at maxBodies once removals have made room.
    void retryBodiesOverCapacity(Scene& scene, const AssetManager& assets);
    // Creates, removes and moves the scene's character controllers for one
    // step, before the bodies are simulated. A controller with an invalid
    // capsule is skipped and reported once.
    void stepCharacters(Scene& scene, float deltaSeconds);
    // The ground distance CharacterControllerComponent::groundDistance reports.
    float probeGround(Entity entity, const CharacterControllerComponent& controller, const glm::vec3& position,
                      uint32_t layer) const;

    class Impl;
    std::unique_ptr<Impl> m_impl;
    PhysicsStats m_stats;
};
