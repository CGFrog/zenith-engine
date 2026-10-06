#pragma once

#include "../assets/Assets.hpp"
#include "../reflection/Reflection.hpp"

#include <compare>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <type_traits>
#include <variant>

enum class PhysicsMotionType {
    Static,
    Kinematic,
    Dynamic,
};

inline constexpr uint32_t maxPhysicsLayers = 32;

// A collision layer: an index into the project's layer table
// (PhysicsLayerTable), below maxPhysicsLayers. Which layers collide is the
// table's collision matrix. Static, Dynamic, and Player are the built-in
// layers every table starts with.
struct PhysicsLayer {
    uint32_t index{0};

    static const PhysicsLayer Static;
    static const PhysicsLayer Dynamic;
    static const PhysicsLayer Player;

    auto operator<=>(const PhysicsLayer&) const = default;
};

inline constexpr PhysicsLayer PhysicsLayer::Static{0};
inline constexpr PhysicsLayer PhysicsLayer::Dynamic{1};
inline constexpr PhysicsLayer PhysicsLayer::Player{2};

// Serialized as its index.
template <> struct ReflectedFieldKind<PhysicsLayer> {
    static constexpr FieldKind kind = FieldKind::UInt32;
};

static_assert(std::is_standard_layout_v<PhysicsLayer> && sizeof(PhysicsLayer) == sizeof(uint32_t),
              "Reflection stores a PhysicsLayer as its index");

struct BoxCollider {
    glm::vec3 halfExtents{0.5f};
};

struct SphereCollider {
    float radius{0.5f};
};

struct CapsuleCollider {
    float halfHeight{0.5f};
    float radius{0.5f};
};

// The exact triangles of a mesh: for static bodies only.
struct TriangleMeshCollider {
    MeshHandle mesh{};
};

// The smallest convex shape around a mesh's vertices: any motion type, so a
// dynamic prop can match its mesh. Concave parts are filled in.
struct ConvexHullCollider {
    MeshHandle mesh{};
};

// Add new alternatives at the end: the editor's shape list follows this order.
using ColliderShape =
    std::variant<BoxCollider, SphereCollider, CapsuleCollider, TriangleMeshCollider, ConvexHullCollider>;

// The mesh a mesh-backed shape (triangle mesh or convex hull) uses; nullopt
// for the primitive shapes.
inline std::optional<MeshHandle> colliderMesh(const ColliderShape& shape) {
    if (const auto* triangles = std::get_if<TriangleMeshCollider>(&shape)) {
        return triangles->mesh;
    }
    if (const auto* hull = std::get_if<ConvexHullCollider>(&shape)) {
        return hull->mesh;
    }
    return std::nullopt;
}

// A collision shape and where it sits on its entity. The body is at the
// entity's world position and rotation; the offset places the shape in the
// entity's local axes (colliderLocalTransform).
struct ColliderComponent {
    ColliderShape shape{BoxCollider{}};
    // In the entity's local axes, before its world scale (so a scaled
    // entity moves the shape with its mesh).
    glm::vec3 offsetPosition{0.0f};
    glm::quat offsetRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Whether the collider's shape sits away from its entity's origin or is
// turned relative to it.
inline bool hasColliderOffset(const ColliderComponent& collider) {
    return collider.offsetPosition != glm::vec3(0.0f) || collider.offsetRotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

// Where a collider's shape sits in its body's space (the entity's world
// position and rotation, unscaled), given the entity's world scale (absolute,
// per axis). The one convention physics bodies, navigation geometry, and the
// editor's gizmos share: the offset position is multiplied by the scale, then
// the offset rotation turns the shape. The shape's own dimensions are still
// multiplied by the scale on the shape's axes, as without an offset.
struct ColliderPlacement {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

inline ColliderPlacement colliderLocalTransform(const ColliderComponent& collider, const glm::vec3& scale) {
    return {.position = collider.offsetPosition * scale, .rotation = glm::normalize(collider.offsetRotation)};
}

struct RigidBodyComponent {
    PhysicsMotionType motionType{PhysicsMotionType::Static};
    PhysicsLayer layer{PhysicsLayer::Static};
    float mass{1.0f};
    float friction{0.5f};
    float restitution{0.0f};
    float linearDamping{0.05f};
    float angularDamping{0.05f};
    float gravityFactor{1.0f};
    bool sensor{false};
    bool allowSleeping{true};
    bool continuousCollision{false};
    bool lockRotation{false};
};
