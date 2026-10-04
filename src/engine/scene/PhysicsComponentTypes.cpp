// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added. Presentation hints mirror the
// ranges the inspector has always offered.

#include "PhysicsComponentTypes.hpp"

#include "ComponentTypeHelpers.hpp"
#include "ComponentTypes.hpp"

#include <nlohmann/json.hpp>

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
} // namespace

ComponentType colliderComponentType() {
    return ComponentTypeBuilder<ColliderComponent>("collider", "Collider")
        .category("Physics")
        .codec(
            [](const ColliderComponent& collider, const ComponentSaveContext& context, Json& output) {
                if (const auto* triangleMesh = std::get_if<TriangleMeshCollider>(&collider.shape)) {
                    Json reference;
                    if (SceneDocumentResult result =
                            meshReference(context.assets, triangleMesh->mesh, context.options, reference);
                        !result) {
                        return result;
                    }
                    output = {{"type", "triangle_mesh"}, {"asset", std::move(reference)}};
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
                } else if (type == "triangle_mesh") {
                    MeshHandle mesh;
                    if (SceneDocumentResult result = readMesh(input.at("asset"), context, mesh); !result) {
                        return result;
                    }
                    collider.shape = TriangleMeshCollider{mesh};
                } else {
                    return SceneDocumentResult{.error = "Unsupported collider type: " + type};
                }
                return SceneDocumentResult{.success = true};
            })
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
                    .add("layer", &C::layer)
                    .options({{"static", "Static", static_cast<int64_t>(PhysicsLayer::Static)},
                              {"dynamic", "Dynamic", static_cast<int64_t>(PhysicsLayer::Dynamic)},
                              {"player", "Player", static_cast<int64_t>(PhysicsLayer::Player)}},
                             EnumEncoding::Integer)
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
        .build();
}
