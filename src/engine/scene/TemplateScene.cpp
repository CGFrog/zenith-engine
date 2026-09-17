#include "TemplateScene.hpp"

#include "../Engine.hpp"

#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

SceneResult createTemplateScene(Engine& engine) {
    const auto map = engine.assets().loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "basic_map.gltf");
    if (!map) {
        return {.error = map.error};
    }

    Scene& scene = engine.scene();
    const Entity mapRoot = scene.createEntity("Gameplay Room");
    if (const SceneResult result = scene.instantiateModel(mapRoot, map.handle, engine.assets()); !result) {
        return result;
    }

    const auto& rootRelationship = scene.registry().get<RelationshipComponent>(mapRoot);
    for (Entity child : rootRelationship.children) {
        if (!scene.registry().any_of<MeshRendererComponent>(child)) {
            continue;
        }
        scene.registry().emplace<ColliderComponent>(child, BoxCollider{glm::vec3(1.0f)});
        scene.registry().emplace<RigidBodyComponent>(child);
    }

    const ModelAsset* model = engine.assets().model(map.handle);
    if (!model || model->nodes.empty() || !model->nodes.front().mesh) {
        return {.error = "Gameplay room does not contain a reusable prop mesh"};
    }
    const MeshHandle cubeMesh = model->nodes.front().mesh;
    const glm::vec3 propPositions[]{
        {-2.5f, 0.7f, 1.0f}, {-2.5f, 1.55f, 1.0f}, {-2.5f, 2.4f, 1.0f},
        {2.2f, 0.7f, -1.2f}, {2.9f, 0.7f, -1.2f}, {2.55f, 1.55f, -1.2f},
    };
    for (size_t i = 0; i < std::size(propPositions); ++i) {
        const Entity prop = scene.createEntity("Physics Prop " + std::to_string(i + 1));
        scene.attachMesh(prop, cubeMesh);
        scene.registry().get<TransformComponent>(prop).local =
            glm::translate(glm::mat4(1.0f), propPositions[i]) * glm::scale(glm::mat4(1.0f), glm::vec3(0.35f));
        scene.registry().emplace<ColliderComponent>(prop, BoxCollider{glm::vec3(1.0f)});
        RigidBodyComponent body;
        body.motionType = PhysicsMotionType::Dynamic;
        body.layer = PhysicsLayer::Dynamic;
        body.mass = 8.0f + static_cast<float>(i) * 2.0f;
        body.friction = 0.65f;
        body.restitution = i == 0 ? 0.55f : 0.1f;
        body.continuousCollision = i == 0;
        scene.registry().emplace<RigidBodyComponent>(prop, body);
    }

    const Entity player = scene.createEntity("Player");
    scene.registry().get<TransformComponent>(player).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.2f, 4.0f));
    CharacterControllerComponent controller;
    const Entity camera = scene.createEntity("Player Camera");
    controller.camera = camera;
    scene.registry().emplace<CharacterControllerComponent>(player, controller);
    scene.registry().emplace<ColliderComponent>(player, CapsuleCollider{controller.halfHeight, controller.radius});
    RigidBodyComponent playerBody;
    playerBody.motionType = PhysicsMotionType::Dynamic;
    playerBody.layer = PhysicsLayer::Player;
    playerBody.mass = 80.0f;
    playerBody.friction = 0.0f;
    playerBody.angularDamping = 1.0f;
    playerBody.continuousCollision = true;
    scene.registry().emplace<RigidBodyComponent>(player, playerBody);

    scene.registry().emplace<CameraComponent>(camera);
    scene.setParent(camera, player);
    scene.registry().get<TransformComponent>(camera).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, controller.halfHeight + controller.radius, 0.0f));
    return {.success = true};
}
