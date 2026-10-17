#pragma once

// CharacterControllerComponent: a walking character moved by a kinematic controller (steps, slopes, ground snapping).

#include "PhysicsComponents.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

// How a character controller moves (CharacterControllerComponent::motion).
enum class CharacterMotion : uint8_t {
    Walking,
    Free,
};

// A walking character that PhysicsWorld moves with a kinematic character
// controller instead of a rigid body: it climbs steps, refuses slopes steeper
// than its limit, sticks to the floor going down, and rides moving
// platforms. The capsule is centred on the entity's origin and stands along
// world Y; the entity keeps its own rotation, which the controller never
// changes. An entity with this component gets no rigid body, even if it also
// has ColliderComponent and RigidBodyComponent.
//
// PhysicsWorld also gives the character a kinematic inner body on its layer,
// so rays, shape casts, sensors and contacts see it as its entity.
//
// Lengths are metres, speeds metres per second, angles radians.
struct CharacterControllerComponent {
    float radius{0.4f};
    // Half the cylinder's height, excluding the end caps.
    float halfHeight{0.5f};
    PhysicsLayer layer{PhysicsLayer::Player};
    // The steepest ground the character stands on and walks up.
    float slopeLimitRadians{0.8726646f};
    // The tallest step the character walks onto without jumping.
    float stepHeight{0.35f};
    // How far below its feet the character looks for ground: it sticks to
    // ground within this distance when walking down, and reports the
    // distance in groundDistance.
    float groundProbeDistance{0.25f};

    float moveSpeed{4.0f};
    float sprintMultiplier{1.75f};
    // Upward speed at the start of a jump.
    float jumpSpeed{5.5f};
    // Rate, per second, at which horizontal velocity approaches desiredVelocity.
    float acceleration{30.0f};
    // Fraction of acceleration available while airborne, 0 to 1.
    float airControl{0.25f};

    // The input actions the engine's zenith.character_input system reads to
    // drive this character: a 2D move axis, and jump and sprint buttons. An
    // empty move action leaves the character to code (for example an AI).
    std::string moveAction{"Move"};
    std::string jumpAction{"Jump"};
    std::string sprintAction{"Sprint"};

    // Intent, written by gameplay before each fixed step and not serialized.
    // desiredVelocity is world-space and horizontal (y is ignored), relative
    // to the ground the character stands on. A jump request is consumed by
    // the next step, whether or not the character could jump.
    glm::vec3 desiredVelocity{0.0f};
    bool jumpRequested{false};
    // Walking falls under gravity, sticks to the ground, and jumps. Free
    // (swimming, flying, climbing) ignores gravity and jumps: the character
    // approaches desiredVelocity horizontally and desiredVerticalSpeed
    // vertically at its full acceleration, still colliding and stepping up.
    CharacterMotion motion{CharacterMotion::Walking};
    // Metres per second upward; used only in Free motion. Kept apart from
    // desiredVelocity, which zenith.character_input rewrites every frame.
    float desiredVerticalSpeed{0.0f};

    // State, written by each PhysicsWorld::step and not serialized.
    bool grounded{false};
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
    // The velocity of the ground under the character (a moving platform).
    glm::vec3 groundVelocity{0.0f};
    glm::vec3 velocity{0.0f};
    // From the bottom of the capsule down to the ground the probe found, or
    // negative when there is none within groundProbeDistance.
    float groundDistance{-1.0f};
};
