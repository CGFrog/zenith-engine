#define GLM_ENABLE_EXPERIMENTAL
#include "PhysicsWorld.hpp"
#include "JoltDebugDraw.hpp"
#include "JoltRuntime.hpp"
#include "engine/diagnostics/OverflowEpisode.hpp"

#include <Jolt/Jolt.h>

// Jolt/Jolt.h defines configuration macros required by all other Jolt headers.
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr JPH::BroadPhaseLayer broadPhaseStatic{0};
constexpr JPH::BroadPhaseLayer broadPhaseMoving{1};

uint32_t bodyKey(JPH::BodyID id) {
    return id.GetIndexAndSequenceNumber();
}

// The contactEventPrecedes key of a contact between two bodies.
auto contactOrderKey(ContactEventType type, SceneEntityId first, SceneEntityId second, const glm::vec3& normal) {
    return std::tuple(std::min(first, second), std::max(first, second), type, first, normal.x, normal.y, normal.z);
}

// Either order of two bodies.
uint64_t pairKey(uint32_t first, uint32_t second) {
    const auto [low, high] = std::minmax(first, second);
    return (static_cast<uint64_t>(high) << 32u) | low;
}

// A Jolt object layer is a PhysicsLayer index with the moving flag in its low
// bit, so one named layer holds both static and moving bodies while Jolt keeps
// them in its static and moving broad-phase trees.
JPH::ObjectLayer objectLayer(uint32_t layerIndex, bool moving) {
    return static_cast<JPH::ObjectLayer>(layerIndex * 2u + (moving ? 1u : 0u));
}

PhysicsLayer layerOf(JPH::ObjectLayer layer) {
    return {static_cast<uint32_t>(layer) / 2u};
}

bool movingLayer(JPH::ObjectLayer layer) {
    return (layer & 1u) != 0;
}

Entity entityFromUserData(uint64_t userData) {
    return static_cast<Entity>(static_cast<std::underlying_type_t<Entity>>(userData));
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

// Triangle-mesh shapes by (mesh, scale), so identical static mesh colliders
// share one bounding-volume tree. Keys hold the mesh generation, so a
// collected or recycled mesh never matches; entries for meshes that are no
// longer live are dropped once the asset revision changes. Bodies keep their
// own references, so dropping an entry never invalidates a body.
class MeshShapeCache {
  public:
    JPH::RefConst<JPH::Shape> find(MeshHandle mesh, const glm::vec3& scale, const AssetManager& assets) {
        prune(assets);
        const auto found = m_shapes.find(key(mesh, scale));
        return found != m_shapes.end() ? found->second : nullptr;
    }

    void insert(MeshHandle mesh, const glm::vec3& scale, JPH::RefConst<JPH::Shape> shape) {
        m_shapes.insert_or_assign(key(mesh, scale), std::move(shape));
    }

    size_t size() const {
        return m_shapes.size();
    }

    void clear() {
        m_shapes.clear();
        m_assetRevision = 0;
    }

    // Drops the entries of meshes that are no longer live; only scans after
    // the asset revision changed.
    void prune(const AssetManager& assets) {
        if (assets.revision() == m_assetRevision) {
            return;
        }
        m_assetRevision = assets.revision();
        std::erase_if(m_shapes, [&](const auto& entry) {
            return assets.mesh(MeshHandle{entry.first.index, entry.first.generation}) == nullptr;
        });
    }

  private:
    struct Key {
        uint32_t index{0};
        uint32_t generation{0};
        // The scale in steps of 1e-5, so bodies whose decomposed scales differ
        // only by rounding (rotated copies) share a shape.
        std::array<int64_t, 3> scale{};

        auto operator<=>(const Key&) const = default;
    };

    static int64_t quantize(float scale) {
        return std::llround(static_cast<double>(scale) * 1.0e5);
    }

    static Key key(MeshHandle mesh, const glm::vec3& scale) {
        return {mesh.index, mesh.generation, {quantize(scale.x), quantize(scale.y), quantize(scale.z)}};
    }

    std::map<Key, JPH::RefConst<JPH::Shape>> m_shapes;
    uint64_t m_assetRevision{0};
};
} // namespace

class PhysicsWorld::Impl {
  public:
    struct BodyState {
        JPH::BodyID id;
        SceneEntityId sceneId;
        bool sensor{false};
        glm::vec3 scale{1.0f};
        PhysicsMotionType motionType{PhysicsMotionType::Static};
        glm::vec3 previousPosition{0.0f};
        glm::vec3 currentPosition{0.0f};
        glm::quat previousRotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::quat currentRotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool activeLastStep{false};
    };

    // A CharacterControllerComponent's Jolt character, with the capsule it
    // was built from (a change rebuilds it) and the positions interpolation
    // blends between.
    struct CharacterState {
        JPH::Ref<JPH::CharacterVirtual> character;
        SceneEntityId sceneId;
        uint32_t innerBodyKey{0};
        float radius{0.0f};
        float halfHeight{0.0f};
        uint32_t layer{0};
        float slopeLimit{0.0f};
        // Horizontal velocity relative to the ground, approaching the
        // desired velocity at the component's acceleration.
        glm::vec3 horizontalVelocity{0.0f};
        glm::vec3 previousPosition{0.0f};
        glm::vec3 currentPosition{0.0f};
        // The position last written to the entity's transform; any other
        // position there means gameplay moved (teleported) the character.
        glm::vec3 writtenPosition{0.0f};
    };

    class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
      public:
        uint32_t GetNumBroadPhaseLayers() const override {
            return 2;
        }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
            return movingLayer(layer) ? broadPhaseMoving : broadPhaseStatic;
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
            return layer == broadPhaseStatic ? "Static" : "Moving";
        }
#endif
    };

    class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
      public:
        explicit ObjectPairFilter(const Impl& owner)
            : m_owner(owner) {}

        bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override {
            return m_owner.config.layers.collides(layerOf(first), layerOf(second));
        }

      private:
        const Impl& m_owner;
    };

    class ObjectBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
      public:
        explicit ObjectBroadPhaseFilter(const Impl& owner)
            : m_owner(owner) {}

        // Both trees hold bodies of every layer, so a layer that collides
        // with any layer may meet bodies in either tree.
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer) const override {
            return m_owner.config.layers.mask(layerOf(layer)) != 0;
        }

      private:
        const Impl& m_owner;
    };

    struct PendingContact {
        ContactEventType type;
        JPH::BodyID first;
        JPH::BodyID second;
        glm::vec3 normal;
        // Whether either body is a sensor; unknown (false) for End.
        bool sensor{false};
        // On the first body's surface; zero for End.
        glm::vec3 point{0.0f};
        // Resolved after the step (orderPendingContacts); zero for a body
        // that is gone or has no scene id.
        SceneEntityId firstId;
        SceneEntityId secondId;
    };

    // A sensor and a body touching it, counted per touching shape pair. Jolt
    // drops the contacts of a body that falls asleep; such a pair stays,
    // dormant, until the body wakes (and Jolt adds its contacts again) or
    // is removed.
    struct SensorPair {
        uint32_t contacts{0};
        bool dormant{false};
        uint32_t sensorBody{0};
        uint32_t otherBody{0};
        Entity sensor{entt::null};
        Entity other{entt::null};
        SceneEntityId sensorId;
        SceneEntityId otherId;
    };

    class Contacts final : public JPH::ContactListener {
      public:
        explicit Contacts(Impl& owner)
            : m_owner(owner) {}

        // Called from Jolt's worker threads.
        void OnContactAdded(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override {
            add(ContactEventType::Begin, first.GetID(), second.GetID(), fromJolt(manifold.mWorldSpaceNormal),
                first.IsSensor() || second.IsSensor(), contactPoint(manifold));
        }

        void OnContactPersisted(const JPH::Body& first, const JPH::Body& second, const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override {
            add(ContactEventType::Persist, first.GetID(), second.GetID(), fromJolt(manifold.mWorldSpaceNormal),
                first.IsSensor() || second.IsSensor(), contactPoint(manifold));
        }

        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            add(ContactEventType::End, pair.GetBody1ID(), pair.GetBody2ID(), {}, false, {});
        }

      private:
        // The manifold's first point on the first body, or its base offset
        // when it has none.
        static glm::vec3 contactPoint(const JPH::ContactManifold& manifold) {
            const JPH::RVec3 point = manifold.mRelativeContactPointsOn1.empty()
                                         ? manifold.mBaseOffset
                                         : manifold.GetWorldSpaceContactPointOn1(0);
            return {static_cast<float>(point.GetX()), static_cast<float>(point.GetY()),
                    static_cast<float>(point.GetZ())};
        }

        void add(ContactEventType type, JPH::BodyID first, JPH::BodyID second, const glm::vec3& normal, bool sensor,
                 const glm::vec3& point) {
            std::scoped_lock lock(m_owner.contactMutex);
            m_owner.pendingContacts.push_back(
                {.type = type, .first = first, .second = second, .normal = normal, .sensor = sensor, .point = point});
        }

        Impl& m_owner;
    };

    Impl()
        : pairFilter(*this)
        , broadPhaseFilter(*this)
        , contacts(*this) {}

    // Removes the entity's body, reporting the end of every sensor overlap it
    // was part of (Jolt reports no contact removal for a removed body).
    void removeBody(Entity entity) {
        const auto existing = bodies.find(entity);
        if (existing == bodies.end()) {
            return;
        }
        const uint32_t key = bodyKey(existing->second.id);
        JPH::BodyInterface& interface = system->GetBodyInterface();
        interface.RemoveBody(existing->second.id);
        interface.DestroyBody(existing->second.id);
        bodyEntities.erase(key);
        bodies.erase(existing);
        endSensorPairs(key);
    }

    // Removes the entity's character and its inner body, reporting the end of
    // its sensor overlaps.
    void removeCharacter(Entity entity) {
        const auto existing = characters.find(entity);
        if (existing == characters.end()) {
            return;
        }
        const uint32_t key = existing->second.innerBodyKey;
        // The character removes and destroys its inner body.
        characters.erase(existing);
        bodyEntities.erase(key);
        endSensorPairs(key);
    }

    void removeCharacters() {
        std::vector<Entity> entities;
        entities.reserve(characters.size());
        for (const auto& [entity, _] : characters) {
            entities.push_back(entity);
        }
        removeCharactersInOrder(std::move(entities));
    }

    // Removes every character, in scene id order so that Jolt reuses the
    // freed inner body ids the same way every run.
    void removeCharactersInOrder(std::vector<Entity> entities) {
        std::ranges::sort(entities, {}, [this](Entity entity) { return characters.at(entity).sceneId; });
        for (const Entity entity : entities) {
            removeCharacter(entity);
        }
    }

    // Puts the step's contacts, which Jolt's worker threads reported in any
    // order, in contactEventPrecedes order (body ids break the remaining ties,
    // such as bodies without scene ids).
    void orderPendingContacts() {
        const auto sceneIdOfBody = [this](JPH::BodyID body) {
            const auto entity = bodyEntities.find(bodyKey(body));
            return entity != bodyEntities.end() ? sceneIdOf(entity->second) : SceneEntityId{};
        };
        for (PendingContact& pending : pendingContacts) {
            pending.firstId = sceneIdOfBody(pending.first);
            pending.secondId = sceneIdOfBody(pending.second);
        }
        std::ranges::sort(pendingContacts, [](const PendingContact& first, const PendingContact& second) {
            // The point last, so several manifolds of one pair (sub-shapes)
            // keep one order too.
            return std::tuple(contactOrderKey(first.type, first.firstId, first.secondId, first.normal),
                              bodyKey(first.first), bodyKey(first.second), first.point.x, first.point.y,
                              first.point.z) <
                   std::tuple(contactOrderKey(second.type, second.firstId, second.secondId, second.normal),
                              bodyKey(second.first), bodyKey(second.second), second.point.x, second.point.y,
                              second.point.z);
        });
    }

    // Reports the end of every sensor overlap the body was part of.
    void endSensorPairs(uint32_t key) {
        for (auto pair = sensorPairs.begin(); pair != sensorPairs.end();) {
            if (pair->second.sensorBody == key || pair->second.otherBody == key) {
                pendingTriggerEvents.push_back(exited(pair->second));
                pair = sensorPairs.erase(pair);
            } else {
                ++pair;
            }
        }
    }

    // Whether either body is a moving body that is asleep (static bodies are
    // never active).
    bool eitherAsleep(uint32_t firstKey, uint32_t secondKey) const {
        const JPH::BodyInterface& interface = system->GetBodyInterface();
        for (const uint32_t key : {firstKey, secondKey}) {
            const auto entity = bodyEntities.find(key);
            if (entity == bodyEntities.end()) {
                return false;
            }
            // A character's inner body never sleeps.
            const auto body = bodies.find(entity->second);
            if (body == bodies.end()) {
                continue;
            }
            const BodyState& state = body->second;
            if (state.motionType != PhysicsMotionType::Static && !interface.IsActive(state.id)) {
                return true;
            }
        }
        return false;
    }

    static TriggerEvent exited(const SensorPair& pair) {
        return {.type = TriggerEventType::Exited,
                .sensor = pair.sensor,
                .other = pair.other,
                .sensorId = pair.sensorId,
                .otherId = pair.otherId};
    }

    // Turns the step's sensor contacts (in orderPendingContacts order, so a
    // pair's Begin counts before its End) into enter and exit events, after
    // the exits of bodies removed since the last step.
    void updateTriggers() {
        const auto removalExits = static_cast<std::ptrdiff_t>(pendingTriggerEvents.size());
        std::ranges::sort(pendingTriggerEvents, triggerEventPrecedes);
        for (const PendingContact& pending : pendingContacts) {
            if (pending.type == ContactEventType::Persist) {
                continue;
            }
            const uint32_t firstKey = bodyKey(pending.first);
            const uint32_t secondKey = bodyKey(pending.second);
            const uint64_t key = pairKey(firstKey, secondKey);
            if (pending.type == ContactEventType::End) {
                const auto pair = sensorPairs.find(key);
                if (pair == sensorPairs.end() || pair->second.contacts == 0 || --pair->second.contacts > 0) {
                    continue;
                }
                if (eitherAsleep(firstKey, secondKey)) {
                    pair->second.dormant = true;
                } else {
                    pendingTriggerEvents.push_back(exited(pair->second));
                    sensorPairs.erase(pair);
                }
                continue;
            }
            if (!pending.sensor) {
                continue;
            }
            const auto first = bodyEntities.find(firstKey);
            const auto second = bodyEntities.find(secondKey);
            if (first == bodyEntities.end() || second == bodyEntities.end()) {
                continue;
            }
            SensorPair& pair = sensorPairs[key];
            if (pair.dormant) {
                // Woke up still inside.
                pair.dormant = false;
                ++pair.contacts;
                continue;
            }
            if (pair.contacts++ > 0) {
                continue;
            }
            const bool firstIsSensor = isSensor(first->second);
            const auto& sensor = firstIsSensor ? *first : *second;
            const auto& other = firstIsSensor ? *second : *first;
            pair.sensorBody = sensor.first;
            pair.otherBody = other.first;
            pair.sensor = sensor.second;
            pair.other = other.second;
            pair.sensorId = sceneIdOf(sensor.second);
            pair.otherId = sceneIdOf(other.second);
            pendingTriggerEvents.push_back({.type = TriggerEventType::Entered,
                                            .sensor = pair.sensor,
                                            .other = pair.other,
                                            .sensorId = pair.sensorId,
                                            .otherId = pair.otherId});
        }
        std::sort(pendingTriggerEvents.begin() + removalExits, pendingTriggerEvents.end(), triggerEventPrecedes);
        triggerEvents.swap(pendingTriggerEvents);
        pendingTriggerEvents.clear();
    }

    // Characters are never sensors.
    bool isSensor(Entity entity) const {
        const auto body = bodies.find(entity);
        return body != bodies.end() && body->second.sensor;
    }

    SceneEntityId sceneIdOf(Entity entity) const {
        if (const auto body = bodies.find(entity); body != bodies.end()) {
            return body->second.sceneId;
        }
        const auto character = characters.find(entity);
        return character != characters.end() ? character->second.sceneId : SceneEntityId{};
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
    // Every body's entity by body key, characters' inner bodies included.
    std::unordered_map<uint32_t, Entity> bodyEntities;
    std::unordered_map<Entity, CharacterState> characters;
    std::vector<PendingContact> pendingContacts;
    std::vector<ContactEvent> contactEvents;
    std::unordered_map<uint64_t, SensorPair> sensorPairs;
    // Exits of bodies removed since the last step; the step appends its own
    // events and publishes them all as triggerEvents.
    std::vector<TriggerEvent> pendingTriggerEvents;
    std::vector<TriggerEvent> triggerEvents;
    std::mutex contactMutex;
    const Scene* observedScene{nullptr};
    uint32_t stepsUntilAudit{0};
    LogChannel log;
    // Entities whose body or character could not be built: skipped by step,
    // reported once, and retried when they change or the next audit runs.
    // capacityRejections counts the bodies rejected because maxBodies was
    // reached, which are reported once per episode instead of per entity.
    std::unordered_map<Entity, bool> rejectedBodies;
    uint32_t capacityRejections{0};
    std::unordered_set<Entity> rejectedCharacters;
    OverflowEpisode bodyCapacity;
    OverflowEpisode contactCapacity;
    MeshShapeCache meshShapes;

    void forgetRejectedBody(Entity entity) {
        const auto rejected = rejectedBodies.find(entity);
        if (rejected == rejectedBodies.end()) {
            return;
        }
        capacityRejections -= rejected->second ? 1u : 0u;
        rejectedBodies.erase(rejected);
    }
};

PhysicsWorld::PhysicsWorld()
    : PhysicsWorld(nullptr) {}

PhysicsWorld::PhysicsWorld(Logger* logger)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->log = LogChannel(logger, "physics");
}

PhysicsWorld::~PhysicsWorld() {
    shutdown();
}

PhysicsResult PhysicsWorld::init(const PhysicsWorldConfig& config) {
    if (m_impl->initialized) {
        return {.success = true};
    }
    if (config.fixedTimeStep <= 0.0f || config.maxSubSteps == 0) {
        return {.error = "Physics configuration requires a positive fixed step and at least one substep"};
    }
    if (const std::string error = config.layers.validate(); !error.empty()) {
        return {.error = error};
    }
    if (config.maxBodies == 0 || config.maxBodies > JPH::PhysicsSystem::cMaxBodiesLimit || config.maxBodyPairs == 0 ||
        config.maxBodyPairs > JPH::PhysicsSystem::cMaxBodyPairsLimit || config.maxContactConstraints == 0 ||
        config.maxContactConstraints > JPH::PhysicsSystem::cMaxContactConstraintsLimit) {
        return {.error = "Physics configuration requires non-zero body, body pair, and contact constraint capacities "
                         "within Jolt's limits"};
    }
    // Jolt aborts the process when a step outgrows its temporary allocator.
    if (config.temporaryAllocatorBytes / contactConstraintTemporaryBytes < config.maxContactConstraints) {
        return {.error = "Physics temporaryAllocatorBytes is too small for maxContactConstraints (each needs " +
                         std::to_string(contactConstraintTemporaryBytes) + " bytes per step)"};
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
    // Cached shapes are Jolt objects; release them while its runtime lives.
    m_impl->meshShapes.clear();
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

    m_impl->sensorPairs.clear();
    m_impl->pendingTriggerEvents.clear();
    m_impl->triggerEvents.clear();
    m_impl->rejectedBodies.clear();
    m_impl->capacityRejections = 0;
    m_impl->rejectedCharacters.clear();
    if (!m_impl->initialized) {
        m_impl->characters.clear();
        m_impl->bodies.clear();
        m_impl->bodyEntities.clear();
        m_impl->pendingContacts.clear();
        m_impl->contactEvents.clear();
        m_impl->observedScene = nullptr;
        m_impl->stepsUntilAudit = 0;
        return;
    }

    m_impl->removeCharacters();
    // In scene id order, so Jolt reuses the freed body ids the same way every run.
    std::vector<std::pair<SceneEntityId, JPH::BodyID>> removed;
    for (const auto& [_, state] : m_impl->bodies) {
        removed.emplace_back(state.sceneId, state.id);
    }
    std::ranges::sort(removed, {}, [](const auto& body) { return std::pair(body.first, bodyKey(body.second)); });
    JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
    for (const auto& [_, id] : removed) {
        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
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
// The convex hull of the mesh's vertices at scale. Not cached: a hull is
// small next to a bounding-volume tree, and each body's scale differs.
JPH::RefConst<JPH::Shape> createConvexHullShape(const ConvexHullCollider& collider, const glm::vec3& scale,
                                                const AssetManager& assets, std::string& error) {
    const MeshAsset* mesh = assets.mesh(collider.mesh);
    if (mesh == nullptr) {
        error = "Convex hull collider references an invalid mesh";
        return nullptr;
    }
    JPH::Array<JPH::Vec3> points;
    points.reserve(mesh->vertices.size());
    for (const Vertex& vertex : mesh->vertices) {
        points.push_back(toJolt(vertex.position * scale));
    }
    if (points.size() < 4) {
        error = "Convex hull collider needs a mesh with at least four vertices";
        return nullptr;
    }
    JPH::ConvexHullShapeSettings settings(points);
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        error = std::string("Convex hull collider could not be built: ") + result.GetError().c_str();
        return nullptr;
    }
    return result.Get();
}

JPH::RefConst<JPH::Shape> createShape(const ColliderComponent& collider, const glm::vec3& scale,
                                      const AssetManager& assets, MeshShapeCache& meshShapes, std::string& error) {
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
            } else if constexpr (std::is_same_v<Shape, ConvexHullCollider>) {
                return createConvexHullShape(shape, scale, assets, error);
            } else {
                const MeshAsset* mesh = assets.mesh(shape.mesh);
                if (!mesh) {
                    error = "Triangle mesh collider references an invalid mesh";
                    return nullptr;
                }
                if (JPH::RefConst<JPH::Shape> cached = meshShapes.find(shape.mesh, scale, assets)) {
                    return cached;
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
                meshShapes.insert(shape.mesh, scale, result.Get());
                return result.Get();
            }
        },
        collider.shape);
}

// Moves a character's entity to position, keeping its rotation and scale.
void writeCharacterPosition(Scene& scene, Entity entity, const glm::vec3& position) {
    entt::registry& registry = scene.registry();
    auto& transform = registry.get<TransformComponent>(entity);
    glm::mat4 world = transform.world;
    world[3] = glm::vec4(position, 1.0f);
    const Entity parent = registry.get<RelationshipComponent>(entity).parent;
    transform.world = world;
    transform.local = parent != entt::null && registry.valid(parent)
                          ? glm::inverse(registry.get<TransformComponent>(parent).world) * world
                          : world;
    scene.markTransformDirty(entity);
}

// Names an entity in warnings: its name when it has one, and its scene id.
std::string describeEntity(const Scene& scene, Entity entity) {
    const NameComponent* name = scene.registry().try_get<NameComponent>(entity);
    const std::string label = name != nullptr && !name->name.empty() ? "'" + name->name + "'" : "an unnamed entity";
    return label + " (scene id " + std::to_string(scene.id(entity).value) + ")";
}

// The capacities a Jolt update error says were exhausted, and the config
// fields that raise them.
std::string describeUpdateError(JPH::EPhysicsUpdateError error) {
    const auto has = [error](JPH::EPhysicsUpdateError flag) {
        return (static_cast<uint32_t>(error) & static_cast<uint32_t>(flag)) != 0;
    };
    std::string description;
    const auto append = [&description](std::string_view text) {
        description += description.empty() ? "" : ", ";
        description += text;
    };
    if (has(JPH::EPhysicsUpdateError::BodyPairCacheFull)) {
        append("body pairs (maxBodyPairs)");
    }
    if (has(JPH::EPhysicsUpdateError::ManifoldCacheFull)) {
        append("contact manifolds (maxContactConstraints)");
    }
    if (has(JPH::EPhysicsUpdateError::ContactConstraintsFull)) {
        append("contact constraints (maxContactConstraints)");
    }
    return description.empty() ? "an unknown capacity" : description;
}

// Whether the entity should have a rigid body: a character controller replaces
// one.
bool hasBodyComponents(const Scene& scene, Entity entity) {
    const entt::registry& registry = scene.registry();
    return registry.valid(entity) &&
           registry.all_of<TransformComponent, ColliderComponent, RigidBodyComponent>(entity) &&
           !registry.all_of<CharacterControllerComponent>(entity);
}
} // namespace

PhysicsResult PhysicsWorld::refreshBody(Entity entity, Scene& scene, const AssetManager& assets) {
    scene.updateWorldTransforms();
    PhysicsResult result = refreshBodyFromWorld(entity, scene, assets);
    if (result) {
        m_impl->forgetRejectedBody(entity);
    }
    return result;
}

PhysicsResult PhysicsWorld::refreshBodyFromWorld(Entity entity, Scene& scene, const AssetManager& assets) {
    if (!m_impl->initialized) {
        return {.error = "Physics world is not initialized"};
    }
    m_impl->removeBody(entity);
    if (!hasBodyComponents(scene, entity)) {
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
    JPH::RefConst<JPH::Shape> shape = createShape(collider, scale, assets, m_impl->meshShapes, error);
    if (!shape) {
        return {.error = std::move(error)};
    }
    if (hasColliderOffset(collider)) {
        // Outside the mesh shape cache: the cached shape stays shareable by
        // colliders with other offsets.
        const ColliderPlacement placement = colliderLocalTransform(collider, scale);
        shape = new JPH::RotatedTranslatedShape(toJolt(placement.position), toJolt(placement.rotation), shape);
    }

    JPH::EMotionType motion = JPH::EMotionType::Static;
    if (rigidBody.motionType == PhysicsMotionType::Kinematic) {
        motion = JPH::EMotionType::Kinematic;
    } else if (rigidBody.motionType == PhysicsMotionType::Dynamic) {
        motion = JPH::EMotionType::Dynamic;
    }
    // A layer the table lacks would collide with nothing; Dynamic is the
    // documented stand-in.
    const uint32_t layer =
        m_impl->config.layers.contains(rigidBody.layer) ? rigidBody.layer.index : PhysicsLayer::Dynamic.index;
    JPH::BodyCreationSettings settings(shape, JPH::RVec3(position.x, position.y, position.z), toJolt(rotation), motion,
                                       objectLayer(layer, motion != JPH::EMotionType::Static));
    settings.mFriction = std::max(0.0f, rigidBody.friction);
    settings.mRestitution = std::clamp(rigidBody.restitution, 0.0f, 1.0f);
    settings.mLinearDamping = std::max(0.0f, rigidBody.linearDamping);
    settings.mAngularDamping = std::max(0.0f, rigidBody.angularDamping);
    settings.mGravityFactor = rigidBody.gravityFactor;
    settings.mIsSensor = rigidBody.sensor;
    // Lets a static or kinematic sensor also detect kinematic bodies.
    settings.mCollideKinematicVsNonDynamic = rigidBody.sensor;
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
    m_impl->bodies.emplace(entity, Impl::BodyState{.id = id,
                                                   .sceneId = scene.id(entity),
                                                   .sensor = rigidBody.sensor,
                                                   .scale = scale,
                                                   .motionType = rigidBody.motionType,
                                                   .previousPosition = position,
                                                   .currentPosition = position,
                                                   .previousRotation = rotation,
                                                   .currentRotation = rotation});
    m_impl->bodyEntities.emplace(bodyKey(id), entity);
    return {.success = true};
}

void PhysicsWorld::refreshOrSkipBody(Entity entity, Scene& scene, const AssetManager& assets) {
    Impl& impl = *m_impl;
    const PhysicsResult result = refreshBodyFromWorld(entity, scene, assets);
    if (result) {
        impl.forgetRejectedBody(entity);
        return;
    }
    // A full body manager is reported once per episode (see step); any other
    // failure is the entity's own and is reported once per entity.
    const bool atCapacity = impl.system->GetNumBodies() >= impl.config.maxBodies;
    const auto [rejected, firstRejection] = impl.rejectedBodies.try_emplace(entity, atCapacity);
    if (!firstRejection) {
        return;
    }
    if (atCapacity) {
        ++impl.capacityRejections;
        return;
    }
    impl.log.warning("Physics skipped the body of " + describeEntity(scene, entity) + ": " + result.error);
}

void PhysicsWorld::retryBodiesOverCapacity(Scene& scene, const AssetManager& assets) {
    Impl& impl = *m_impl;
    if (impl.capacityRejections == 0 || impl.system->GetNumBodies() >= impl.config.maxBodies) {
        return;
    }
    // In entity order, so the bodies that get the freed room are deterministic.
    std::vector<Entity> waiting;
    for (const auto& [entity, atCapacity] : impl.rejectedBodies) {
        if (atCapacity) {
            waiting.push_back(entity);
        }
    }
    std::ranges::sort(waiting, {}, [](Entity entity) { return entt::to_integral(entity); });
    for (const Entity entity : waiting) {
        if (impl.system->GetNumBodies() >= impl.config.maxBodies) {
            return;
        }
        impl.forgetRejectedBody(entity);
        if (hasBodyComponents(scene, entity)) {
            refreshOrSkipBody(entity, scene, assets);
        }
    }
}

namespace {
// Farther than this from where the character last put its entity, the entity
// was moved by gameplay; closer is rounding through a parent's transform.
constexpr float characterTeleportDistance = 1.0e-4f;
} // namespace

void PhysicsWorld::stepCharacters(Scene& scene, float deltaSeconds) {
    Impl& impl = *m_impl;
    entt::registry& registry = scene.registry();
    std::vector<Entity> removed;
    for (const auto& [entity, _] : impl.characters) {
        if (!registry.valid(entity) || !registry.all_of<TransformComponent, CharacterControllerComponent>(entity)) {
            removed.push_back(entity);
        }
    }
    impl.removeCharactersInOrder(std::move(removed));
    std::erase_if(impl.rejectedCharacters, [&registry](Entity entity) {
        return !registry.valid(entity) || !registry.all_of<TransformComponent, CharacterControllerComponent>(entity);
    });

    const JPH::Vec3 gravity = toJolt(impl.config.gravity);
    for (const Entity entity : registry.view<TransformComponent, CharacterControllerComponent>()) {
        auto& controller = registry.get<CharacterControllerComponent>(entity);
        const glm::vec3 position(registry.get<TransformComponent>(entity).world[3]);
        // A layer the table lacks would collide with nothing.
        const uint32_t layer =
            impl.config.layers.contains(controller.layer) ? controller.layer.index : PhysicsLayer::Player.index;
        const float slopeLimit = std::clamp(controller.slopeLimitRadians, 0.0f, glm::radians(89.0f));

        auto existing = impl.characters.find(entity);
        if (existing != impl.characters.end() &&
            (existing->second.radius != controller.radius || existing->second.halfHeight != controller.halfHeight ||
             existing->second.layer != layer || existing->second.slopeLimit != slopeLimit)) {
            impl.removeCharacter(entity);
            existing = impl.characters.end();
        }
        if (existing == impl.characters.end()) {
            if (!(controller.radius > 0.0f) || !(controller.halfHeight > 0.0f)) {
                if (impl.rejectedCharacters.insert(entity).second) {
                    impl.log.warning("Physics skipped the character controller of " + describeEntity(scene, entity) +
                                     ": its capsule radius and half height must be positive");
                }
                continue;
            }
            impl.rejectedCharacters.erase(entity);
            JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
            settings->mShape = new JPH::CapsuleShape(controller.halfHeight, controller.radius);
            settings->mInnerBodyShape = settings->mShape;
            settings->mInnerBodyLayer = objectLayer(layer, true);
            settings->mMaxSlopeAngle = slopeLimit;
            // Only contacts below the lower cap's centre hold the character up.
            settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), controller.halfHeight);
            JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
                settings, JPH::RVec3(position.x, position.y, position.z), JPH::Quat::sIdentity(),
                static_cast<uint64_t>(entt::to_integral(entity)), impl.system.get());
            const uint32_t innerBodyKey = bodyKey(character->GetInnerBodyID());
            existing = impl.characters
                           .emplace(entity, Impl::CharacterState{.character = std::move(character),
                                                                 .sceneId = scene.id(entity),
                                                                 .innerBodyKey = innerBodyKey,
                                                                 .radius = controller.radius,
                                                                 .halfHeight = controller.halfHeight,
                                                                 .layer = layer,
                                                                 .slopeLimit = slopeLimit,
                                                                 .previousPosition = position,
                                                                 .currentPosition = position,
                                                                 .writtenPosition = position})
                           .first;
            impl.bodyEntities[innerBodyKey] = entity;
        } else if (glm::distance(position, existing->second.writtenPosition) > characterTeleportDistance) {
            existing->second.character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
            existing->second.previousPosition = position;
            existing->second.currentPosition = position;
        }

        Impl::CharacterState& state = existing->second;
        JPH::CharacterVirtual& character = *state.character;
        // The ground may have changed speed since the last step (a platform).
        character.UpdateGroundVelocity();
        const glm::vec3 groundVelocity = fromJolt(character.GetGroundVelocity());
        const float verticalSpeed = character.GetLinearVelocity().GetY();
        const bool onGround = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        const bool movingTowardGround = verticalSpeed - groundVelocity.y < 0.1f;

        const glm::vec3 desired = fromJolt(character.CancelVelocityTowardsSteepSlopes(
            JPH::Vec3(controller.desiredVelocity.x, 0.0f, controller.desiredVelocity.z)));
        // Free motion (swimming, flying) has full control in every direction
        // and no gravity, jumping, or sticking to the floor.
        const bool free = controller.motion == CharacterMotion::Free;
        const float control = onGround || free ? 1.0f : std::clamp(controller.airControl, 0.0f, 1.0f);
        const float blend = std::min(1.0f, std::max(controller.acceleration, 0.0f) * control * deltaSeconds);
        state.horizontalVelocity = glm::mix(state.horizontalVelocity, glm::vec3(desired.x, 0.0f, desired.z), blend);

        glm::vec3 velocity(0.0f, verticalSpeed, 0.0f);
        if (free) {
            velocity.y = glm::mix(verticalSpeed, controller.desiredVerticalSpeed, blend);
        } else if (onGround && movingTowardGround) {
            velocity = groundVelocity;
            if (controller.jumpRequested) {
                velocity.y += controller.jumpSpeed;
            }
        }
        controller.jumpRequested = false;
        if (!free) {
            velocity += impl.config.gravity * deltaSeconds;
        }
        velocity += state.horizontalVelocity;
        character.SetLinearVelocity(toJolt(velocity));

        const JPH::ObjectLayer characterLayer = objectLayer(layer, true);
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mStickToFloorStepDown =
            free ? JPH::Vec3::sZero() : JPH::Vec3(0.0f, -std::max(controller.groundProbeDistance, 0.0f), 0.0f);
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, std::max(controller.stepHeight, 0.0f), 0.0f);
        character.ExtendedUpdate(deltaSeconds, free ? JPH::Vec3::sZero() : gravity, update,
                                 impl.system->GetDefaultBroadPhaseLayerFilter(characterLayer),
                                 impl.system->GetDefaultLayerFilter(characterLayer), JPH::BodyFilter{},
                                 JPH::ShapeFilter{}, *impl.tempAllocator);

        const JPH::RVec3 moved = character.GetPosition();
        state.previousPosition = state.currentPosition;
        state.currentPosition = {static_cast<float>(moved.GetX()), static_cast<float>(moved.GetY()),
                                 static_cast<float>(moved.GetZ())};
        state.writtenPosition = state.currentPosition;
        writeCharacterPosition(scene, entity, state.currentPosition);

        controller.grounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        controller.groundNormal =
            controller.grounded ? fromJolt(character.GetGroundNormal()) : glm::vec3(0.0f, 1.0f, 0.0f);
        controller.groundVelocity = controller.grounded ? fromJolt(character.GetGroundVelocity()) : glm::vec3(0.0f);
        controller.velocity = fromJolt(character.GetLinearVelocity());
        controller.groundDistance = probeGround(entity, controller, state.currentPosition, layer);
    }
}

float PhysicsWorld::probeGround(Entity entity, const CharacterControllerComponent& controller,
                                const glm::vec3& position, uint32_t layer) const {
    // A sphere slightly narrower than the capsule's lower cap, starting a
    // little above the capsule's bottom so that it does not begin inside the
    // ground the character stands on.
    const float inset = controller.radius * 0.1f;
    const float distance = std::max(controller.groundProbeDistance, 0.0f) + inset;
    const Entity self[] = {entity};
    PhysicsShapeHit hit;
    if (controller.groundProbeDistance <= 0.0f ||
        !shapeCast(PhysicsQuerySphere{controller.radius - inset},
                   {.position = position - glm::vec3(0.0f, controller.halfHeight, 0.0f)}, glm::vec3(0.0f, -1.0f, 0.0f),
                   distance, hit, {.layerMask = m_impl->config.layers.mask(PhysicsLayer{layer}), .ignore = self})) {
        return -1.0f;
    }
    return std::max(0.0f, hit.fraction * distance - inset);
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
    m_impl->meshShapes.prune(assets);
    scene.updateWorldTransforms();

    const auto transformsDone = std::chrono::steady_clock::now();
    m_stats.transformMilliseconds = milliseconds(start, transformsDone);
    JPH::BodyInterface& interface = m_impl->system->GetBodyInterface();
    const auto removeBody = [&](Entity entity) { m_impl->removeBody(entity); };

    const bool newScene = m_impl->observedScene != &scene;
    if (newScene) {
        m_impl->observedScene = &scene;
        m_impl->stepsUntilAudit = 0;
        // Characters and rejections hold entity handles of the previous scene.
        m_impl->removeCharacters();
        m_impl->rejectedBodies.clear();
        m_impl->capacityRejections = 0;
        m_impl->rejectedCharacters.clear();
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
        if (event.operation == PhysicsLifecycleOperation::Destroy || !hasBodyComponents(scene, event.entity)) {
            removeBody(event.entity);
            m_impl->forgetRejectedBody(event.entity);
        } else {
            refreshOrSkipBody(event.entity, scene, assets);
        }
    }
    scene.retirePhysicsLifecycleEvents();
    retryBodiesOverCapacity(scene, assets);

    const bool audit = newScene || (m_impl->config.debugAuditInterval != 0 && m_impl->stepsUntilAudit == 0);
    if (audit) {
        auto view = scene.registry().view<TransformComponent, ColliderComponent, RigidBodyComponent>(
            entt::exclude<CharacterControllerComponent>);
        for (Entity entity : view) {
            ++m_stats.auditedEntities;
            if (!m_impl->bodies.contains(entity)) {
                refreshOrSkipBody(entity, scene, assets);
            }
        }
        // In scene id order, so Jolt reuses the freed body ids the same way
        // every run.
        std::vector<std::pair<SceneEntityId, Entity>> stale;
        for (const auto& [entity, state] : m_impl->bodies) {
            if (!hasBodyComponents(scene, entity)) {
                stale.emplace_back(state.sceneId, entity);
            }
        }
        std::ranges::sort(stale);
        for (const auto& [_, entity] : stale) {
            removeBody(entity);
        }
        for (auto iterator = m_impl->rejectedBodies.begin(); iterator != m_impl->rejectedBodies.end();) {
            const Entity entity = iterator->first;
            ++iterator;
            if (!hasBodyComponents(scene, entity)) {
                m_impl->forgetRejectedBody(entity);
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
    // After the kinematic bodies have their velocities, so characters ride
    // moving platforms; their inner bodies' contacts land in this step.
    stepCharacters(scene, deltaSeconds);
    const auto syncDone = std::chrono::steady_clock::now();
    m_stats.synchronizeInMilliseconds = milliseconds(transformsDone, syncDone);
    const JPH::EPhysicsUpdateError updateError =
        m_impl->system->Update(deltaSeconds, 1, m_impl->tempAllocator.get(), m_impl->jobs.get());
    const auto simulationDone = std::chrono::steady_clock::now();
    m_stats.simulationMilliseconds = milliseconds(syncDone, simulationDone);
    // Every Jolt update error is an exhausted capacity: the step completed
    // without the contacts that did not fit.
    m_stats.contactCapacityExceeded = updateError != JPH::EPhysicsUpdateError::None;
    if (m_impl->contactCapacity.update(m_stats.contactCapacityExceeded)) {
        m_impl->log.warning("Physics ran out of " + describeUpdateError(updateError) +
                            "; some contacts are not resolved until the load drops (raise them in PhysicsWorldConfig)");
    }
    if (m_impl->bodyCapacity.update(m_impl->capacityRejections > 0)) {
        m_impl->log.warning("Physics reached its body capacity of " + std::to_string(m_impl->config.maxBodies) +
                            " (PhysicsWorldConfig::maxBodies); " + std::to_string(m_impl->capacityRejections) +
                            " bodies are skipped until bodies are removed");
    }
    m_stats.skippedBodies = static_cast<uint32_t>(m_impl->rejectedBodies.size() + m_impl->rejectedCharacters.size());

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
    m_impl->orderPendingContacts();
    for (const Impl::PendingContact& pending : m_impl->pendingContacts) {
        const auto first = m_impl->bodyEntities.find(bodyKey(pending.first));
        const auto second = m_impl->bodyEntities.find(bodyKey(pending.second));
        if (first != m_impl->bodyEntities.end() && second != m_impl->bodyEntities.end()) {
            m_impl->contactEvents.push_back({.type = pending.type,
                                             .first = first->second,
                                             .second = second->second,
                                             .normal = pending.normal,
                                             .firstId = pending.firstId,
                                             .secondId = pending.secondId,
                                             .point = pending.point});
        }
    }
    m_impl->updateTriggers();
    m_stats.contactEvents = static_cast<uint32_t>(m_impl->contactEvents.size());
    m_stats.synchronizeOutMilliseconds = milliseconds(simulationDone, std::chrono::steady_clock::now());
    return {.success = true};
}

void PhysicsWorld::interpolate(Scene& scene, float alpha) const {
    const float amount = std::clamp(alpha, 0.0f, 1.0f);
    // A parented body's local transform needs its parent's current world
    // transform, which systems may have changed since the last step. Bodies
    // without a parent (the usual case) need no update first: their world
    // and local transforms are written together below, and the caller's next
    // updateWorldTransforms propagates them to their children.
    bool parentWorldsCurrent = false;
    for (const auto& [entity, state] : m_impl->bodies) {
        if (state.motionType != PhysicsMotionType::Dynamic || !state.activeLastStep ||
            !scene.registry().valid(entity)) {
            continue;
        }
        const glm::vec3 position = glm::mix(state.previousPosition, state.currentPosition, amount);
        const glm::quat rotation = glm::slerp(state.previousRotation, state.currentRotation, amount);
        const glm::mat4 world = composeTransform(position, rotation, state.scale);
        const Entity parent = scene.registry().get<RelationshipComponent>(entity).parent;
        const bool parented = parent != entt::null && scene.registry().valid(parent);
        if (parented && !parentWorldsCurrent) {
            scene.updateWorldTransforms();
            parentWorldsCurrent = true;
        }
        auto& transform = scene.registry().get<TransformComponent>(entity);
        transform.world = world;
        transform.local =
            parented ? glm::inverse(scene.registry().get<TransformComponent>(parent).world) * world : world;
        scene.markTransformDirty(entity);
    }
    for (auto& [entity, state] : m_impl->characters) {
        if (scene.registry().valid(entity)) {
            state.writtenPosition = glm::mix(state.previousPosition, state.currentPosition, amount);
            writeCharacterPosition(scene, entity, state.writtenPosition);
        }
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

namespace {
// The layers a query may hit; a layer index is the object layer without its
// moving bit.
class QueryLayerFilter final : public JPH::ObjectLayerFilter {
  public:
    explicit QueryLayerFilter(uint32_t layerMask)
        : m_layerMask(layerMask) {}

    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        const uint32_t index = layerOf(layer).index;
        return index < maxPhysicsLayers && (m_layerMask & (1u << index)) != 0;
    }

  private:
    uint32_t m_layerMask;
};

// Skips sensors (unless asked for) and the ignored entities, which bodies
// carry in their user data.
class QueryBodyFilter final : public JPH::BodyFilter {
  public:
    explicit QueryBodyFilter(const PhysicsQueryFilter& filter)
        : m_filter(filter) {}

    bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (body.IsSensor() && !m_filter.hitSensors) {
            return false;
        }
        return std::ranges::find(m_filter.ignore, entityFromUserData(body.GetUserData())) == m_filter.ignore.end();
    }

  private:
    const PhysicsQueryFilter& m_filter;
};

JPH::RefConst<JPH::Shape> createQueryShape(const PhysicsQueryShape& shape) {
    return std::visit(
        [](const auto& value) -> JPH::RefConst<JPH::Shape> {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, PhysicsQuerySphere>) {
                return value.radius > 0.0f ? new JPH::SphereShape(value.radius) : nullptr;
            } else if constexpr (std::is_same_v<Value, PhysicsQueryCapsule>) {
                return value.radius > 0.0f && value.halfHeight > 0.0f
                           ? new JPH::CapsuleShape(value.halfHeight, value.radius)
                           : nullptr;
            } else {
                const float smallest = std::min({value.halfExtents.x, value.halfExtents.y, value.halfExtents.z});
                // Jolt rounds a box's edges by a convex radius no larger than
                // its smallest half extent.
                return smallest > 0.0f
                           ? new JPH::BoxShape(toJolt(value.halfExtents), std::min(JPH::cDefaultConvexRadius, smallest))
                           : nullptr;
            }
        },
        shape);
}

JPH::RMat44 queryTransform(const PhysicsQueryPose& pose) {
    return JPH::RMat44::sRotationTranslation(toJolt(glm::normalize(pose.rotation)),
                                             JPH::RVec3(pose.position.x, pose.position.y, pose.position.z));
}

bool usableDirection(const glm::vec3& direction, float distance) {
    return distance > 0.0f && std::isfinite(distance) && glm::dot(direction, direction) > 0.0f;
}
} // namespace

PhysicsResult PhysicsWorld::setLayers(const PhysicsLayerTable& layers) {
    if (const std::string error = layers.validate(); !error.empty()) {
        return {.error = error};
    }
    if (!m_impl->bodies.empty()) {
        return {.error = "Physics layers can only change while the world has no bodies"};
    }
    m_impl->config.layers = layers;
    return {.success = true};
}

bool PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, float distance, PhysicsRayHit& hit,
                           const PhysicsQueryFilter& filter) const {
    hit = {};
    if (!m_impl->initialized || !usableDirection(direction, distance)) {
        return false;
    }
    const glm::vec3 rayVector = glm::normalize(direction) * distance;
    JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), toJolt(rayVector));
    JPH::RayCastResult result;
    if (!m_impl->system->GetNarrowPhaseQuery().CastRay(ray, result, {}, QueryLayerFilter(filter.layerMask),
                                                       QueryBodyFilter(filter))) {
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

bool PhysicsWorld::raycastAll(const glm::vec3& origin, const glm::vec3& direction, float distance,
                              std::vector<PhysicsRayHit>& hits, const PhysicsQueryFilter& filter) const {
    hits.clear();
    if (!m_impl->initialized || !usableDirection(direction, distance)) {
        return false;
    }
    const glm::vec3 rayVector = glm::normalize(direction) * distance;
    JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), toJolt(rayVector));
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    m_impl->system->GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings(), collector, {},
                                                  QueryLayerFilter(filter.layerMask), QueryBodyFilter(filter));
    collector.Sort();
    for (const JPH::RayCastResult& result : collector.mHits) {
        const auto entity = m_impl->bodyEntities.find(bodyKey(result.mBodyID));
        // Sorted, so a body's first hit is its nearest.
        if (entity == m_impl->bodyEntities.end() ||
            std::ranges::any_of(hits, [&](const PhysicsRayHit& hit) { return hit.entity == entity->second; })) {
            continue;
        }
        JPH::BodyLockRead lock(m_impl->system->GetBodyLockInterface(), result.mBodyID);
        hits.push_back({.entity = entity->second,
                        .position = origin + rayVector * result.mFraction,
                        .normal = lock.Succeeded() ? fromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(
                                                         result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)))
                                                   : glm::vec3(0.0f),
                        .fraction = result.mFraction});
    }
    return !hits.empty();
}

bool PhysicsWorld::shapeCast(const PhysicsQueryShape& shape, const PhysicsQueryPose& start, const glm::vec3& direction,
                             float distance, PhysicsShapeHit& hit, const PhysicsQueryFilter& filter) const {
    hit = {};
    const JPH::RefConst<JPH::Shape> queryShape = createQueryShape(shape);
    if (!m_impl->initialized || queryShape == nullptr || !usableDirection(direction, distance)) {
        return false;
    }
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        queryShape, JPH::Vec3::sReplicate(1.0f), queryTransform(start), toJolt(glm::normalize(direction) * distance));
    JPH::ShapeCastSettings settings;
    // A shape that starts inside a body reports the deepest point, at
    // fraction zero.
    settings.mReturnDeepestPoint = true;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    m_impl->system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {},
                                                    QueryLayerFilter(filter.layerMask), QueryBodyFilter(filter));
    if (!collector.HadHit()) {
        return false;
    }
    const JPH::ShapeCastResult& result = collector.mHit;
    const auto entity = m_impl->bodyEntities.find(bodyKey(result.mBodyID2));
    if (entity == m_impl->bodyEntities.end()) {
        return false;
    }
    const JPH::Vec3 axis = result.mPenetrationAxis;
    hit.entity = entity->second;
    hit.position = fromJolt(JPH::Vec3(result.mContactPointOn2));
    hit.normal = axis.LengthSq() > 0.0f ? fromJolt(-axis.Normalized()) : glm::vec3(0.0f);
    hit.fraction = result.mFraction;
    return true;
}

bool PhysicsWorld::overlap(const PhysicsQueryShape& shape, const PhysicsQueryPose& pose, std::vector<Entity>& entities,
                           const PhysicsQueryFilter& filter) const {
    entities.clear();
    const JPH::RefConst<JPH::Shape> queryShape = createQueryShape(shape);
    if (!m_impl->initialized || queryShape == nullptr) {
        return false;
    }
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    m_impl->system->GetNarrowPhaseQuery().CollideShape(queryShape, JPH::Vec3::sReplicate(1.0f), queryTransform(pose),
                                                       JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector, {},
                                                       QueryLayerFilter(filter.layerMask), QueryBodyFilter(filter));
    for (const JPH::CollideShapeResult& result : collector.mHits) {
        const auto entity = m_impl->bodyEntities.find(bodyKey(result.mBodyID2));
        if (entity != m_impl->bodyEntities.end() && std::ranges::find(entities, entity->second) == entities.end()) {
            entities.push_back(entity->second);
        }
    }
    return !entities.empty();
}

bool PhysicsWorld::closestPoint(const glm::vec3& point, float maxDistance, PhysicsClosestPoint& closest,
                                const PhysicsQueryFilter& filter) const {
    closest = {};
    if (!m_impl->initialized || !(maxDistance >= 0.0f) || !std::isfinite(maxDistance)) {
        return false;
    }
    // A tiny probe sphere reports every body within maxDistance of its
    // surface, with the nearest point on each.
    constexpr float probeRadius = 1.0e-3f;
    const JPH::RefConst<JPH::Shape> probe = new JPH::SphereShape(probeRadius);
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = maxDistance;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    m_impl->system->GetNarrowPhaseQuery().CollideShape(
        probe, JPH::Vec3::sReplicate(1.0f), queryTransform({.position = point}), settings, JPH::RVec3::sZero(),
        collector, {}, QueryLayerFilter(filter.layerMask), QueryBodyFilter(filter));
    bool found = false;
    for (const JPH::CollideShapeResult& result : collector.mHits) {
        const auto entity = m_impl->bodyEntities.find(bodyKey(result.mBodyID2));
        // A negative depth is the gap between the probe's surface and the body.
        const float distance = std::max(0.0f, probeRadius - result.mPenetrationDepth);
        if (entity == m_impl->bodyEntities.end() || distance > maxDistance || (found && distance >= closest.distance)) {
            continue;
        }
        closest = {
            .entity = entity->second, .position = fromJolt(JPH::Vec3(result.mContactPointOn2)), .distance = distance};
        found = true;
    }
    return found;
}

bool contactEventPrecedes(const ContactEvent& first, const ContactEvent& second) {
    return contactOrderKey(first.type, first.firstId, first.secondId, first.normal) <
           contactOrderKey(second.type, second.firstId, second.secondId, second.normal);
}

bool triggerEventPrecedes(const TriggerEvent& first, const TriggerEvent& second) {
    return std::tuple(first.sensorId, first.otherId, first.type) <
           std::tuple(second.sensorId, second.otherId, second.type);
}

std::span<const ContactEvent> PhysicsWorld::contactEvents() const {
    return m_impl->contactEvents;
}

std::span<const TriggerEvent> PhysicsWorld::triggerEvents() const {
    return m_impl->triggerEvents;
}

PhysicsResult PhysicsWorld::drawDebug(DebugDraw& output, const PhysicsDebugDrawOptions& options) const {
    // The default channel, as DebugDraw's own shapes record.
    return drawDebug(output.channel({}), options);
}

PhysicsResult PhysicsWorld::drawDebug(const DebugDrawRecorder& output, const PhysicsDebugDrawOptions& options) const {
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
    renderer->setTarget(output, options.depth);
    m_impl->system->DrawBodies(settings, renderer);
    renderer->setTarget(std::nullopt, options.depth);
    return {.success = true};
}

size_t PhysicsWorld::cachedMeshShapes() const {
    return m_impl->meshShapes.size();
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
