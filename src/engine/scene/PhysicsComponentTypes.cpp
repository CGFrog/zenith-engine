// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added. Presentation hints mirror the
// ranges the inspector has always offered.

#include "PhysicsComponentTypes.hpp"

#include "ComponentTypeHelpers.hpp"
#include "ComponentTypes.hpp"
#include "engine/physics/CharacterController.hpp"
#include "engine/physics/PhysicsLayers.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <optional>
#include <type_traits>
#include <variant>

namespace {
using Json = nlohmann::json;
using component_type_helpers::meshReference;
using component_type_helpers::readMesh;

Json vector3(const glm::vec3& value) {
    return Json::array({value.x, value.y, value.z});
}

bool readVector3(const Json& json, glm::vec3& value) {
    if (!json.is_array() || json.size() != 3) {
        return false;
    }
    value = {json[0].get<float>(), json[1].get<float>(), json[2].get<float>()};
    return true;
}

// A mesh-backed shape's document type name and mesh, or nullopt for a
// primitive shape.
struct MeshShapeRecord {
    const char* type;
    MeshHandle mesh;
};

std::optional<MeshShapeRecord> meshShapeRecord(const ColliderShape& shape) {
    const std::optional<MeshHandle> mesh = colliderMesh(shape);
    if (!mesh) {
        return std::nullopt;
    }
    return MeshShapeRecord{std::holds_alternative<ConvexHullCollider>(shape) ? "convex_hull" : "triangle_mesh", *mesh};
}

// "offset": {"position": [x, y, z], "rotation": [x, y, z, w]}, written only
// when the shape is moved or turned (version 2).
Json colliderOffset(const ColliderComponent& collider) {
    const glm::quat& rotation = collider.offsetRotation;
    return {{"position", vector3(collider.offsetPosition)},
            {"rotation", Json::array({rotation.x, rotation.y, rotation.z, rotation.w})}};
}

SceneDocumentResult readColliderOffset(const Json& input, ColliderComponent& collider) {
    const auto offset = input.find("offset");
    if (offset == input.end()) {
        collider.offsetPosition = glm::vec3(0.0f);
        collider.offsetRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        return {.success = true};
    }
    const auto position = offset->is_object() ? offset->find("position") : offset->end();
    const auto rotation = offset->is_object() ? offset->find("rotation") : offset->end();
    glm::vec3 offsetPosition{0.0f};
    if (position != offset->end() && !readVector3(*position, offsetPosition)) {
        return {.error = "Collider offset position must contain three numbers"};
    }
    glm::quat offsetRotation{1.0f, 0.0f, 0.0f, 0.0f};
    if (rotation != offset->end()) {
        if (!rotation->is_array() || rotation->size() != 4) {
            return {.error = "Collider offset rotation must be a quaternion [x, y, z, w]"};
        }
        offsetRotation = glm::quat((*rotation)[3].get<float>(), (*rotation)[0].get<float>(),
                                   (*rotation)[1].get<float>(), (*rotation)[2].get<float>());
        const float length = glm::length(offsetRotation);
        if (!(length > 1.0e-6f) || !std::isfinite(length)) {
            return {.error = "Collider offset rotation must not be zero"};
        }
        offsetRotation /= length;
    }
    collider.offsetPosition = offsetPosition;
    collider.offsetRotation = offsetRotation;
    return {.success = true};
}
} // namespace

ComponentType colliderComponentType() {
    return ComponentTypeBuilder<ColliderComponent>("collider", "Collider")
        .category("Physics")
        .codec(
            [](const ColliderComponent& collider, const ComponentSaveContext& context, Json& output) {
                if (const std::optional<MeshShapeRecord> meshShape = meshShapeRecord(collider.shape)) {
                    Json reference;
                    if (SceneDocumentResult result =
                            meshReference(context.assets, meshShape->mesh, context.options, reference);
                        !result) {
                        return result;
                    }
                    output = {{"type", meshShape->type}, {"asset", std::move(reference)}};
                    if (hasColliderOffset(collider)) {
                        output["offset"] = colliderOffset(collider);
                    }
                    return SceneDocumentResult{.success = true};
                }
                std::visit(
                    [&output](const auto& shape) {
                        using Shape = std::decay_t<decltype(shape)>;
                        if constexpr (std::is_same_v<Shape, BoxCollider>) {
                            output = {{"type", "box"}, {"half_extents", vector3(shape.halfExtents)}};
                        } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                            output = {{"type", "sphere"}, {"radius", shape.radius}};
                        } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                            output = {{"type", "capsule"}, {"half_height", shape.halfHeight}, {"radius", shape.radius}};
                        }
                    },
                    collider.shape);
                if (hasColliderOffset(collider)) {
                    output["offset"] = colliderOffset(collider);
                }
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, ColliderComponent& collider, const ComponentLoadContext& context) {
                const std::string type = input.at("type").get<std::string>();
                if (type == "box") {
                    BoxCollider box;
                    if (!readVector3(input.at("half_extents"), box.halfExtents)) {
                        return SceneDocumentResult{.error = "Box collider half_extents must contain three numbers"};
                    }
                    collider.shape = box;
                } else if (type == "sphere") {
                    collider.shape = SphereCollider{input.at("radius").get<float>()};
                } else if (type == "capsule") {
                    collider.shape =
                        CapsuleCollider{input.at("half_height").get<float>(), input.at("radius").get<float>()};
                } else if (type == "triangle_mesh" || type == "convex_hull") {
                    MeshHandle mesh;
                    if (SceneDocumentResult result = readMesh(input.at("asset"), context, mesh); !result) {
                        return result;
                    }
                    if (type == "triangle_mesh") {
                        collider.shape = TriangleMeshCollider{mesh};
                    } else {
                        collider.shape = ConvexHullCollider{mesh};
                    }
                } else {
                    return SceneDocumentResult{.error = "Unsupported collider type: " + type};
                }
                return readColliderOffset(input, collider);
            })
        // Version 2: an optional "offset" moves and turns the shape on its
        // entity. Version 1 documents have none, so they need no change.
        .schemaVersion(2)
        .migration(1, [](nlohmann::json&) { return SceneDocumentResult{.success = true}; })
        .build();
}

ComponentType rigidBodyComponentType() {
    using C = RigidBodyComponent;
    return ComponentTypeBuilder<C>("rigid_body", "Rigid Body")
        .category("Physics")
        .fields(Fields<C>()
                    .add("motion_type", &C::motionType)
                    .label("Motion")
                    .options({{"static", "Static", static_cast<int64_t>(PhysicsMotionType::Static)},
                              {"kinematic", "Kinematic", static_cast<int64_t>(PhysicsMotionType::Kinematic)},
                              {"dynamic", "Dynamic", static_cast<int64_t>(PhysicsMotionType::Dynamic)}},
                             EnumEncoding::Integer)
                    // An index into the project's layer table (0 Static, 1
                    // Dynamic, 2 Player by default).
                    .add("layer", &C::layer)
                    .nameList(std::string(physicsLayerNameList))
                    .drag(0.1f, 0.0, static_cast<double>(maxPhysicsLayers - 1))
                    .add("mass", &C::mass)
                    .drag(0.05f, 0.001, 100000.0)
                    .add("friction", &C::friction)
                    .slider(0.0, 1.0)
                    .add("restitution", &C::restitution)
                    .slider(0.0, 1.0)
                    .add("linear_damping", &C::linearDamping)
                    .drag(0.01f, 0.0, 100.0)
                    .add("angular_damping", &C::angularDamping)
                    .drag(0.01f, 0.0, 100.0)
                    .add("gravity_factor", &C::gravityFactor)
                    .drag(0.01f, -10.0, 10.0)
                    .add("sensor", &C::sensor)
                    .add("allow_sleeping", &C::allowSleeping)
                    .add("continuous_collision", &C::continuousCollision)
                    .add("lock_rotation", &C::lockRotation))
        // Version 2: "layer" names any project layer by index, not only
        // the three built-in ones. Their indices are unchanged, so version 1
        // documents need no change.
        .schemaVersion(2)
        .migration(1, [](nlohmann::json&) { return SceneDocumentResult{.success = true}; })
        .validate([](const C& body) {
            return body.layer.index < maxPhysicsLayers
                       ? std::string{}
                       : "Physics layer " + std::to_string(body.layer.index) + " is beyond the last layer, " +
                             std::to_string(maxPhysicsLayers - 1);
        })
        .build();
}

ComponentType characterControllerComponentType() {
    using C = CharacterControllerComponent;
    return ComponentTypeBuilder<C>("character_controller", "Character Controller")
        .category("Physics")
        .fields(Fields<C>()
                    .add("radius", &C::radius)
                    .drag(0.01f, 0.01, 10.0)
                    .format("%.2f m")
                    .tooltip("Capsule radius")
                    .add("half_height", &C::halfHeight)
                    .drag(0.01f, 0.01, 10.0)
                    .format("%.2f m")
                    .tooltip("Half the capsule's cylinder height, excluding the end caps")
                    .add("layer", &C::layer)
                    .nameList(std::string(physicsLayerNameList))
                    .drag(0.1f, 0.0, static_cast<double>(maxPhysicsLayers - 1))
                    .add("slope_limit", &C::slopeLimitRadians)
                    .angle()
                    .slider(0.0, 89.0)
                    .tooltip("The steepest ground the character stands on and walks up")
                    .add("step_height", &C::stepHeight)
                    .drag(0.01f, 0.0, 2.0)
                    .format("%.2f m")
                    .tooltip("The tallest step the character walks onto without jumping")
                    .add("ground_probe_distance", &C::groundProbeDistance)
                    .drag(0.01f, 0.0, 2.0)
                    .format("%.2f m")
                    .tooltip("How far below its feet the character sticks to and looks for ground")
                    .add("move_speed", &C::moveSpeed)
                    .drag(0.05f, 0.0, 100.0)
                    .format("%.2f m/s")
                    .add("sprint_multiplier", &C::sprintMultiplier)
                    .drag(0.01f, 0.0, 10.0)
                    .format("%.2fx")
                    .add("jump_speed", &C::jumpSpeed)
                    .drag(0.05f, 0.0, 100.0)
                    .format("%.2f m/s")
                    .tooltip("Upward speed at the start of a jump")
                    .add("acceleration", &C::acceleration)
                    .drag(0.1f, 0.0, 500.0)
                    .format("%.1f /s")
                    .tooltip("How quickly horizontal velocity approaches the requested velocity")
                    .add("air_control", &C::airControl)
                    .slider(0.0, 1.0)
                    .tooltip("Fraction of acceleration available while airborne")
                    .add("move_action", &C::moveAction)
                    .tooltip("2D input action that walks the character; empty leaves it to code")
                    .add("jump_action", &C::jumpAction)
                    .add("sprint_action", &C::sprintAction))
        // Version 2: the engine's controller replaces the demo's. Look
        // sensitivity, yaw and pitch moved to first_person_camera, and the
        // new fields start at their defaults.
        .schemaVersion(2)
        .migration(1,
                   [](nlohmann::json& component) {
                       const C defaults;
                       for (const char* key : {"look_sensitivity", "yaw", "pitch"}) {
                           component.erase(key);
                       }
                       component.emplace("layer", defaults.layer.index);
                       component.emplace("slope_limit", defaults.slopeLimitRadians);
                       component.emplace("step_height", defaults.stepHeight);
                       component.emplace("ground_probe_distance", defaults.groundProbeDistance);
                       component.emplace("move_action", defaults.moveAction);
                       component.emplace("jump_action", defaults.jumpAction);
                       component.emplace("sprint_action", defaults.sprintAction);
                       return SceneDocumentResult{.success = true};
                   })
        .validate([](const C& controller) -> std::string {
            if (!(controller.radius > 0.0f) || !(controller.halfHeight > 0.0f)) {
                return "character controller capsule dimensions must be positive";
            }
            if (controller.layer.index >= maxPhysicsLayers) {
                return "Physics layer " + std::to_string(controller.layer.index) + " is beyond the last layer, " +
                       std::to_string(maxPhysicsLayers - 1);
            }
            if (!(controller.stepHeight >= 0.0f) || !(controller.groundProbeDistance >= 0.0f) ||
                !(controller.moveSpeed >= 0.0f) || !(controller.sprintMultiplier >= 0.0f) ||
                !(controller.jumpSpeed >= 0.0f) || !(controller.acceleration >= 0.0f)) {
                return "character controller distances, speeds and acceleration must not be negative";
            }
            if (!(controller.airControl >= 0.0f && controller.airControl <= 1.0f)) {
                return "character controller air control must be between 0 and 1";
            }
            return {};
        })
        .build();
}
