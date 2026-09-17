#pragma once

#include "../assets/Assets.hpp"

#include <glm/glm.hpp>
#include <entt/entity/entity.hpp>
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
};

struct CharacterControllerComponent {
    float radius{0.4f};
    float halfHeight{0.5f};
    float moveSpeed{4.0f};
    float sprintMultiplier{1.75f};
    float jumpSpeed{5.5f};
    float acceleration{30.0f};
    float airControl{0.25f};
    float lookSensitivity{0.0025f};
    float yawRadians{0.0f};
    float pitchRadians{0.0f};
    bool grounded{false};
    entt::entity camera{entt::null};
};
