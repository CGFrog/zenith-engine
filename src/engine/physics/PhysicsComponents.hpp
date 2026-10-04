#pragma once

#include "../assets/Assets.hpp"
#include "../reflection/Reflection.hpp"

#include <compare>
#include <cstdint>
#include <glm/glm.hpp>
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

struct TriangleMeshCollider {
    MeshHandle mesh{};
};

using ColliderShape = std::variant<BoxCollider, SphereCollider, CapsuleCollider, TriangleMeshCollider>;

struct ColliderComponent {
    ColliderShape shape{BoxCollider{}};
};

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
