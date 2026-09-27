#pragma once

#include "../assets/Assets.hpp"

#include <glm/glm.hpp>
#include <variant>

enum class PhysicsMotionType {
    Static,
    Kinematic,
    Dynamic,
};

enum class PhysicsLayer : uint16_t {
    Static = 0,
    Dynamic = 1,
    Player = 2,
};

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
