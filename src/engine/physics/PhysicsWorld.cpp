#define GLM_ENABLE_EXPERIMENTAL
#include "PhysicsWorld.hpp"
#include "JoltDebugDraw.hpp"
#include "JoltRuntime.hpp"

#include <Jolt/Jolt.h>

// Jolt/Jolt.h defines configuration macros required by all other Jolt headers.
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
constexpr JPH::BroadPhaseLayer broadPhaseStatic{0};
constexpr JPH::BroadPhaseLayer broadPhaseMoving{1};

uint32_t bodyKey(JPH::BodyID id) {
    return id.GetIndexAndSequenceNumber();
}

JPH::Vec3 toJolt(const glm::vec3& value) {
    return {value.x, value.y, value.z};
}

glm::vec3 fromJolt(JPH::Vec3Arg value) {
    return {value.GetX(), value.GetY(), value.GetZ()};
}

JPH::Quat toJolt(const glm::quat& value) {
    return {value.x, value.y, value.z, value.w};
}

glm::quat fromJolt(JPH::QuatArg value) {
    return {value.GetW(), value.GetX(), value.GetY(), value.GetZ()};
}

bool decomposeTransform(const glm::mat4& matrix, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) {
    glm::vec3 skew;
    glm::vec4 perspective;
    if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective)) {
        return false;
    }
    rotation = glm::normalize(rotation);
    scale = glm::abs(scale);
    return std::isfinite(translation.x) && std::isfinite(rotation.w) &&
           glm::all(glm::greaterThan(scale, glm::vec3(0.0f)));
}

glm::mat4 composeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale) {
    return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}
} // namespace

class PhysicsWorld::Impl {
  public:
    struct BodyState {
        JPH::BodyID id;
        glm::vec3 scale{1.0f};
        PhysicsMotionType motionType{PhysicsMotionType::Static};
        glm::vec3 previousPosition{0.0f};
        glm::vec3 currentPosition{0.0f};
        glm::quat previousRotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::quat currentRotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool activeLastStep{false};
    };

    class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
      public:
        explicit BroadPhaseLayers(const Impl& owner)
            : m_owner(owner) {}

        uint32_t GetNumBroadPhaseLayers() const override {
            return 2;
        }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
            return m_owner.layer(layer).moving ? broadPhaseMoving : broadPhaseStatic;
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
            return layer == broadPhaseStatic ? "Static" : "Moving";
        }
#endif

      private:
        const Impl& m_owner;
    };

    class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
      public:
        explicit ObjectPairFilter(const Impl& owner)
            : m_owner(owner) {}

        bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override {
            const auto& a = m_owner.layer(first);
            const auto& b = m_owner.layer(second);
            return (a.categoryBits & b.collidesWithBits) != 0 && (b.categoryBits & a.collidesWithBits) != 0;
        }

      private:
        const Impl& m_owner;
    };

    class ObjectBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
      public:
        explicit ObjectBroadPhaseFilter(const Impl& owner)
            : m_owner(owner) {}

        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override {
            const auto& source = m_owner.layer(layer);
            for (const PhysicsLayerDefinition& candidate : m_owner.config.layers) {
                const JPH::BroadPhaseLayer candidateBroad = candidate.moving ? broadPhaseMoving : broadPhaseStatic;
                if (candidateBroad == broadPhase && (source.collidesWithBits & candidate.categoryBits) != 0 &&
                    (candidate.collidesWithBits & source.categoryBits) != 0) {
                    return true;
                }
            }
            return false;
        }

      private:
        const Impl& m_owner;
    };

    struct PendingContact {
        ContactEventType type;
        JPH::BodyID first;
        JPH::BodyID second;
        glm::vec3 normal;
    };

    class Contacts final : public JPH::ContactListener {
      public:
        explicit Contacts(Impl& owner)
            : m_owner(owner) {}

        void OnContactAdded(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override {
            add(ContactEventType::Begin, first.GetID(), second.GetID(), fromJolt(manifold.mWorldSpaceNormal));
        }

        void OnContactPersisted(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override {
            add(ContactEventType::Persist, first.GetID(), second.GetID(), fromJolt(manifold.mWorldSpaceNormal));
        }

        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            add(ContactEventType::End, pair.GetBody1ID(), pair.GetBody2ID(), {});
        }

      private:
        void add(ContactEventType type, JPH::BodyID first, JPH::BodyID second, const glm::vec3& normal) {
            std::scoped_lock lock(m_owner.contactMutex);
            m_owner.pendingContacts.push_back({type, first, second, normal});
        }

        Impl& m_owner;
    };

    Impl()
        : broadPhaseLayers(*this)
        , pairFilter(*this)
        , broadPhaseFilter(*this)
        , contacts(*this) {}

    const PhysicsLayerDefinition& layer(JPH::ObjectLayer value) const {
        for (const PhysicsLayerDefinition& definition : config.layers) {
            if (static_cast<uint16_t>(definition.layer) == value) {
                return definition;
            }
        }
        return config.layers.front();
    }

    PhysicsWorldConfig config;
    bool initialized{false};
    // Keeps Jolt's process-wide factory, types, and debug renderer alive
    // while this world is initialized; shared with every other world.
    JoltRuntimeLease runtime;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    BroadPhaseLayers broadPhaseLayers;
    ObjectPairFilter pairFilter;
    ObjectBroadPhaseFilter broadPhaseFilter;
    Contacts contacts;
    std::unordered_map<Entity, BodyState> bodies;
    std::unordered_map<uint32_t, Entity> bodyEntities;
    std::vector<PendingContact> pendingContacts;
    std::vector<ContactEvent> contactEvents;
    std::mutex contactMutex;
    const Scene* observedScene{nullptr};
    uint32_t stepsUntilAudit{0};
};

PhysicsWorld::PhysicsWorld()
    : m_impl(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld() {
    shutdown();
}

PhysicsResult PhysicsWorld::init(const PhysicsWorldConfig& config) {
    if (m_impl->initialized) {
        return {.success = true};
    }
    if (config.layers.empty() || config.fixedTimeStep <= 0.0f || config.maxSubSteps == 0) {
        return {.error = "Physics configuration requires layers, a positive fixed step, and at least one substep"};
    }

    m_impl->config = config;
    m_impl->runtime = JoltRuntimeLease::acquire();

    m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(config.temporaryAllocatorBytes);
    const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t workers = config.workerThreads == 0 ? std::max(1u, hardwareThreads - 1u) : config.workerThreads;
    m_impl->jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                              static_cast<int>(workers));
    m_impl->system = std::make_unique<JPH::PhysicsSystem>();
    m_impl->system->Init(config.maxBodies, 0, config.maxBodyPairs, config.maxContactConstraints,
                         m_impl->broadPhaseLayers, m_impl->broadPhaseFilter, m_impl->pairFilter);
    m_impl->system->SetGravity(toJolt(config.gravity));
    m_impl->system->SetContactListener(&m_impl->contacts);
    m_impl->bodies.reserve(config.maxBodies);
    m_impl->bodyEntities.reserve(config.maxBodies);
    m_impl->pendingContacts.reserve(config.maxContactConstraints);
    m_impl->contactEvents.reserve(config.maxContactConstraints);
    m_impl->initialized = true;
    return {.success = true};
}

void PhysicsWorld::shutdown() {
    if (!m_impl || !m_impl->initialized) {
        return;
    }

    clear();
    m_impl->system.reset();
    m_impl->jobs.reset();
    m_impl->tempAllocator.reset();
    // The last world to shut down destroys Jolt's shared runtime.
    m_impl->runtime.release();
    m_impl->initialized = false;
}

void PhysicsWorld::clear() {
    if (!m_impl) {
        return;
    }

    if (!m_impl->initialized) {
        m_impl->bodies.clear();
        m_impl->bodyEntities.clear();
        m_impl->pendingContacts.clear();
        m_impl->contactEvents.clear();
        m_impl->observedScene = nullptr;
        m_impl->stepsUntilAudit = 0;
        return;
    }

    JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
    for (const auto& [_, state] : m_impl->bodies) {
        bodies.RemoveBody(state.id);
        bodies.DestroyBody(state.id);
    }
    m_impl->bodies.clear();
    m_impl->bodyEntities.clear();
    {
        std::scoped_lock lock(m_impl->contactMutex);
        m_impl->pendingContacts.clear();
    }
    m_impl->contactEvents.clear();
    m_impl->observedScene = nullptr;
    m_impl->stepsUntilAudit = 0;
}

namespace {
JPH::RefConst<JPH::Shape> createShape(const ColliderComponent& collider, const glm::vec3& scale,
                                      const AssetManager& assets, std::string& error) {
    return std::visit(
        [&](const auto& shape) -> JPH::RefConst<JPH::Shape> {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, BoxCollider>) {
                const glm::vec3 extents = shape.halfExtents * scale;
                if (glm::any(glm::lessThanEqual(extents, glm::vec3(0.0f)))) {
                    error = "Box collider half-extents must be positive";
                    return nullptr;
                }
                return new JPH::BoxShape(toJolt(extents));
            } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                const float radius = shape.radius * std::max({scale.x, scale.y, scale.z});
                if (radius <= 0.0f) {
                    error = "Sphere collider radius must be positive";
                    return nullptr;
                }
                return new JPH::SphereShape(radius);
            } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                const float radius = shape.radius * std::max(scale.x, scale.z);
                const float halfHeight = shape.halfHeight * scale.y;
                if (radius <= 0.0f || halfHeight <= 0.0f) {
                    error = "Capsule collider dimensions must be positive";
                    return nullptr;
                }
                return new JPH::CapsuleShape(halfHeight, radius);
            } else {
                const MeshAsset* mesh = assets.mesh(shape.mesh);
                if (!mesh) {
                    error = "Triangle mesh collider references an invalid mesh";
                    return nullptr;
                }
                JPH::TriangleList triangles;
                auto addTriangle = [&](uint32_t a, uint32_t b, uint32_t c) {
                    if (a >= mesh->vertices.size() || b >= mesh->vertices.size() || c >= mesh->vertices.size()) {
                        return;
                    }
                    triangles.emplace_back(toJolt(mesh->vertices[a].position * scale),
                                           toJolt(mesh->vertices[b].position * scale),
                                           toJolt(mesh->vertices[c].position * scale));
                };
                if (mesh->indices.empty()) {
                    for (uint32_t i = 0; i + 2 < mesh->vertices.size(); i += 3) {
                        addTriangle(i, i + 1, i + 2);
                    }
                } else if (mesh->primitives.empty()) {
                    for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
                        addTriangle(mesh->indices[i], mesh->indices[i + 1], mesh->indices[i + 2]);
                    }
                } else {
                    // Only full-detail ranges collide; simplified LOD ranges
                    // follow them in the same index buffer.
                    for (const MeshPrimitive& primitive : mesh->primitives) {
                        const size_t end = std::min<size_t>(
                            static_cast<size_t>(primitive.firstIndex) + primitive.indexCount, mesh->indices.size());
                        for (size_t i = primitive.firstIndex; i + 2 < end; i += 3) {
                            addTriangle(mesh->indices[i], mesh->indices[i + 1], mesh->indices[i + 2]);
                        }
                    }
                }
                if (triangles.empty()) {
                    error = "Triangle mesh collider has no valid triangles";
                    return nullptr;
                }
                JPH::MeshShapeSettings settings(triangles);
                auto result = settings.Create();
                if (result.HasError()) {
                    error = result.GetError().c_str();
                    return nullptr;
                }
                return result.Get();
            }
        },
        collider.shape);
}
} // namespace

PhysicsResult PhysicsWorld::refreshBody(Entity entity, Scene& scene, const AssetManager& assets) {
    scene.updateWorldTransforms();
    return refreshBodyFromWorld(entity, scene, assets);
}

PhysicsResult PhysicsWorld::refreshBodyFromWorld(Entity entity, Scene& scene, const AssetManager& assets) {
    if (!m_impl->initialized) {
        return {.error = "Physics world is not initialized"};
    }
    auto existing = m_impl->bodies.find(entity);
    if (existing != m_impl->bodies.end()) {
        JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
        bodies.RemoveBody(existing->second.id);
        bodies.DestroyBody(existing->second.id);
        m_impl->bodyEntities.erase(bodyKey(existing->second.id));
        m_impl->bodies.erase(existing);
    }
    if (!scene.registry().valid(entity) ||
        !scene.registry().all_of<TransformComponent, ColliderComponent, RigidBodyComponent>(entity)) {
        return {.success = true};
    }

    const auto& transform = scene.registry().get<TransformComponent>(entity);
    const auto& collider = scene.registry().get<ColliderComponent>(entity);
    const auto& rigidBody = scene.registry().get<RigidBodyComponent>(entity);
    if (rigidBody.motionType != PhysicsMotionType::Static &&
        std::holds_alternative<TriangleMeshCollider>(collider.shape)) {
        return {.error = "Triangle mesh colliders are supported only on static bodies"};
    }

    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;
    if (!decomposeTransform(transform.world, position, rotation, scale)) {
        return {.error = "Physics body transform cannot be decomposed"};
    }
    std::string error;
    JPH::RefConst<JPH::Shape> shape = createShape(collider, scale, assets, error);
    if (!shape) {
        return {.error = std::move(error)};
    }

    JPH::EMotionType motion = JPH::EMotionType::Static;
    if (rigidBody.motionType == PhysicsMotionType::Kinematic) {
        motion = JPH::EMotionType::Kinematic;
    } else if (rigidBody.motionType == PhysicsMotionType::Dynamic) {
        motion = JPH::EMotionType::Dynamic;
    }
    JPH::BodyCreationSettings settings(shape, JPH::RVec3(position.x, position.y, position.z), toJolt(rotation), motion,
                                       static_cast<JPH::ObjectLayer>(rigidBody.layer));
    settings.mFriction = std::max(0.0f, rigidBody.friction);
    settings.mRestitution = std::clamp(rigidBody.restitution, 0.0f, 1.0f);
    settings.mLinearDamping = std::max(0.0f, rigidBody.linearDamping);
    settings.mAngularDamping = std::max(0.0f, rigidBody.angularDamping);
    settings.mGravityFactor = rigidBody.gravityFactor;
    settings.mIsSensor = rigidBody.sensor;
    settings.mAllowSleeping = rigidBody.allowSleeping;
    settings.mMotionQuality =
        rigidBody.continuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    settings.mUserData = static_cast<uint64_t>(entt::to_integral(entity));
    if (rigidBody.lockRotation) {
        settings.mAllowedDOFs =
            JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
    }
    if (motion == JPH::EMotionType::Dynamic && rigidBody.mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = rigidBody.mass;
    }

    JPH::BodyID id = m_impl->system->GetBodyInterface().CreateAndAddBody(
        settings, motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        return {.error = "Jolt could not create the physics body"};
    }
    m_impl->bodies.emplace(entity,
                           Impl::BodyState{id, scale, rigidBody.motionType, position, position, rotation, rotation});
    m_impl->bodyEntities.emplace(bodyKey(id), entity);
    return {.success = true};
}

PhysicsResult PhysicsWorld::step(Scene& scene, const AssetManager& assets, float deltaSeconds) {
    m_stats = {};
    const auto start = std::chrono::steady_clock::now();
    const auto milliseconds = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    if (!m_impl->initialized) {
        return {.error = "Physics world is not initialized"};
    }
    scene.updateWorldTransforms();

    const auto transformsDone = std::chrono::steady_clock::now();
    m_stats.transformMilliseconds = milliseconds(start, transformsDone);
    JPH::BodyInterface& interface = m_impl->system->GetBodyInterface();
    const auto removeBody = [&](Entity entity) {
        const auto existing = m_impl->bodies.find(entity);
        if (existing != m_impl->bodies.end()) {
            interface.RemoveBody(existing->second.id);
            interface.DestroyBody(existing->second.id);
            m_impl->bodyEntities.erase(bodyKey(existing->second.id));
            m_impl->bodies.erase(existing);
        }
    };

    const bool newScene = m_impl->observedScene != &scene;
    if (newScene) {
        m_impl->observedScene = &scene;
        m_impl->stepsUntilAudit = 0;
    }
    const std::span<const PhysicsLifecycleEvent> lifecycleEvents = scene.physicsLifecycleEvents();
    for (size_t index = 0; index < lifecycleEvents.size(); ++index) {
        const PhysicsLifecycleEvent& event = lifecycleEvents[index];
        bool superseded = false;
        for (size_t later = index + 1; later < lifecycleEvents.size(); ++later) {
            if (lifecycleEvents[later].entity == event.entity) {
                superseded = true;
                break;
            }
        }
        if (superseded) {
            continue;
        }
        ++m_stats.lifecycleOperations;
        if (event.operation == PhysicsLifecycleOperation::Destroy || !scene.registry().valid(event.entity) ||
            !scene.registry().all_of<TransformComponent, ColliderComponent, RigidBodyComponent>(event.entity)) {
            removeBody(event.entity);
        } else if (PhysicsResult refresh = refreshBodyFromWorld(event.entity, scene, assets); !refresh) {
            return refresh;
        }
    }
    scene.retirePhysicsLifecycleEvents();

    const bool audit = newScene || (m_impl->config.debugAuditInterval != 0 && m_impl->stepsUntilAudit == 0);
    if (audit) {
        auto view = scene.registry().view<TransformComponent, ColliderComponent, RigidBodyComponent>();
        for (Entity entity : view) {
            ++m_stats.auditedEntities;
            if (!m_impl->bodies.contains(entity)) {
                if (PhysicsResult refresh = refreshBodyFromWorld(entity, scene, assets); !refresh) {
                    return refresh;
                }
            }
        }
        for (auto iterator = m_impl->bodies.begin(); iterator != m_impl->bodies.end();) {
            const Entity entity = iterator->first;
            ++iterator;
            if (!scene.registry().valid(entity) ||
                !scene.registry().all_of<TransformComponent, ColliderComponent, RigidBodyComponent>(entity)) {
                removeBody(entity);
            }
        }
        m_impl->stepsUntilAudit = m_impl->config.debugAuditInterval;
    }
    if (m_impl->stepsUntilAudit > 0) {
        --m_impl->stepsUntilAudit;
    }

    for (const auto& [entity, state] : m_impl->bodies) {
        if (state.motionType != PhysicsMotionType::Kinematic) {
            continue;
        }
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 scale;
        if (decomposeTransform(scene.registry().get<TransformComponent>(entity).world, position, rotation, scale)) {
            interface.MoveKinematic(state.id, JPH::RVec3(position.x, position.y, position.z), toJolt(rotation),
                                    deltaSeconds);
        }
    }

    {
        std::scoped_lock lock(m_impl->contactMutex);
        m_impl->pendingContacts.clear();
    }
    const auto syncDone = std::chrono::steady_clock::now();
    m_stats.synchronizeInMilliseconds = milliseconds(transformsDone, syncDone);
    const JPH::EPhysicsUpdateError updateError =
        m_impl->system->Update(deltaSeconds, 1, m_impl->tempAllocator.get(), m_impl->jobs.get());
    const auto simulationDone = std::chrono::steady_clock::now();
    m_stats.simulationMilliseconds = milliseconds(syncDone, simulationDone);
    if (updateError != JPH::EPhysicsUpdateError::None) {
        return {.error = "Jolt reported an error while updating the simulation"};
    }

    for (auto& [entity, state] : m_impl->bodies) {
        ++m_stats.bodies;
        if (state.motionType != PhysicsMotionType::Dynamic || !scene.registry().valid(entity)) {
            continue;
        }
        if (interface.IsActive(state.id)) {
            ++m_stats.activeBodies;
            state.activeLastStep = true;
        } else {
            ++m_stats.sleepingBodies;
            state.activeLastStep = false;
            continue;
        }
        JPH::RVec3 position;
        JPH::Quat rotation;
        interface.GetPositionAndRotation(state.id, position, rotation);
        state.previousPosition = state.currentPosition;
        state.previousRotation = state.currentRotation;
        state.currentPosition = {static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
                                 static_cast<float>(position.GetZ())};
        state.currentRotation = fromJolt(rotation);
        ++m_stats.transformsReadBack;
        glm::mat4 world = composeTransform(state.currentPosition, state.currentRotation, state.scale);
        auto& transform = scene.registry().get<TransformComponent>(entity);
        const auto& relationship = scene.registry().get<RelationshipComponent>(entity);
        transform.world = world;
        transform.local =
            relationship.parent != entt::null && scene.registry().valid(relationship.parent)
                ? glm::inverse(scene.registry().get<TransformComponent>(relationship.parent).world) * world
                : world;
        scene.markTransformDirty(entity);
    }

    m_impl->contactEvents.clear();
    std::scoped_lock lock(m_impl->contactMutex);
    for (const Impl::PendingContact& pending : m_impl->pendingContacts) {
        const auto first = m_impl->bodyEntities.find(bodyKey(pending.first));
        const auto second = m_impl->bodyEntities.find(bodyKey(pending.second));
        if (first != m_impl->bodyEntities.end() && second != m_impl->bodyEntities.end()) {
            m_impl->contactEvents.push_back({pending.type, first->second, second->second, pending.normal});
        }
    }
    m_stats.contactEvents = static_cast<uint32_t>(m_impl->contactEvents.size());
    m_stats.synchronizeOutMilliseconds = milliseconds(simulationDone, std::chrono::steady_clock::now());
    return {.success = true};
}

void PhysicsWorld::interpolate(Scene& scene, float alpha) const {
    const float amount = std::clamp(alpha, 0.0f, 1.0f);
    scene.updateWorldTransforms();
    for (const auto& [entity, state] : m_impl->bodies) {
        if (state.motionType != PhysicsMotionType::Dynamic || !state.activeLastStep ||
            !scene.registry().valid(entity)) {
            continue;
        }
        const glm::vec3 position = glm::mix(state.previousPosition, state.currentPosition, amount);
        const glm::quat rotation = glm::slerp(state.previousRotation, state.currentRotation, amount);
        const glm::mat4 world = composeTransform(position, rotation, state.scale);
        auto& transform = scene.registry().get<TransformComponent>(entity);
        const auto& relationship = scene.registry().get<RelationshipComponent>(entity);
        transform.world = world;
        transform.local =
            relationship.parent != entt::null && scene.registry().valid(relationship.parent)
                ? glm::inverse(scene.registry().get<TransformComponent>(relationship.parent).world) * world
                : world;
        scene.markTransformDirty(entity);
    }
}

bool PhysicsWorld::applyForce(Entity entity, const glm::vec3& force) {
    const auto body = m_impl->bodies.find(entity);
    if (!m_impl->initialized || body == m_impl->bodies.end()) {
        return false;
    }
    m_impl->system->GetBodyInterface().AddForce(body->second.id, toJolt(force));
    return true;
}

bool PhysicsWorld::applyImpulse(Entity entity, const glm::vec3& impulse) {
    const auto body = m_impl->bodies.find(entity);
    if (!m_impl->initialized || body == m_impl->bodies.end()) {
        return false;
    }
    m_impl->system->GetBodyInterface().AddImpulse(body->second.id, toJolt(impulse));
    return true;
}

bool PhysicsWorld::setLinearVelocity(Entity entity, const glm::vec3& velocity) {
    const auto body = m_impl->bodies.find(entity);
    if (!m_impl->initialized || body == m_impl->bodies.end()) {
        return false;
    }
    m_impl->system->GetBodyInterface().SetLinearVelocity(body->second.id, toJolt(velocity));
    return true;
}

glm::vec3 PhysicsWorld::linearVelocity(Entity entity) const {
    const auto body = m_impl->bodies.find(entity);
    if (!m_impl->initialized || body == m_impl->bodies.end()) {
        return {};
    }
    return fromJolt(m_impl->system->GetBodyInterface().GetLinearVelocity(body->second.id));
}

bool PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, float distance,
                           PhysicsRayHit& hit) const {
    if (!m_impl->initialized || distance <= 0.0f || glm::dot(direction, direction) <= 0.0f) {
        return false;
    }
    const glm::vec3 rayVector = glm::normalize(direction) * distance;
    JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), toJolt(rayVector));
    JPH::RayCastResult result;
    if (!m_impl->system->GetNarrowPhaseQuery().CastRay(ray, result)) {
        return false;
    }
    const auto entity = m_impl->bodyEntities.find(bodyKey(result.mBodyID));
    if (entity == m_impl->bodyEntities.end()) {
        return false;
    }
    hit.entity = entity->second;
    hit.fraction = result.mFraction;
    hit.position = origin + rayVector * result.mFraction;
    JPH::BodyLockRead lock(m_impl->system->GetBodyLockInterface(), result.mBodyID);
    hit.normal = lock.Succeeded() ? fromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(
                                        result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)))
                                  : glm::vec3(0.0f);
    return true;
}

std::span<const ContactEvent> PhysicsWorld::contactEvents() const {
    return m_impl->contactEvents;
}

PhysicsResult PhysicsWorld::drawDebug(DebugDraw& output, const PhysicsDebugDrawOptions& options) const {
    if (!m_impl->initialized) {
        return {.error = "Physics debug drawing needs an initialized physics world"};
    }
    // Jolt keeps one debug renderer per process; every world draws through
    // the shared runtime's, which outlives them all.
    JoltDebugDraw* renderer = m_impl->runtime.debugRenderer();
    if (renderer == nullptr) {
        return {.error = "Physics debug drawing found another Jolt debug renderer in this process"};
    }
    JPH::BodyManager::DrawSettings settings;
    settings.mDrawShape = options.shapes;
    settings.mDrawShapeWireframe = true;
    settings.mDrawShapeColor = JPH::BodyManager::EShapeColor::MotionTypeColor;
    settings.mDrawBoundingBox = options.boundingBoxes;
    settings.mDrawCenterOfMassTransform = options.centersOfMass;
    settings.mDrawVelocity = options.velocities;
    renderer->setTarget(&output, options.depth);
    m_impl->system->DrawBodies(settings, renderer);
    renderer->setTarget(nullptr, options.depth);
    return {.success = true};
}

const PhysicsWorldConfig& PhysicsWorld::config() const {
    return m_impl->config;
}

JPH::PhysicsSystem* PhysicsWorld::nativeSystem() {
    return m_impl->system.get();
}

JPH::BodyInterface* PhysicsWorld::nativeBodyInterface() {
    return m_impl->system ? &m_impl->system->GetBodyInterface() : nullptr;
}
