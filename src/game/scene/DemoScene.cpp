#include "DemoScene.hpp"

#include "engine/assets/Assets.hpp"

#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <string>

SceneResult populateDemoScene(Scene& scene, AssetManager& assets, uint32_t benchmarkObjectCount) {
    const auto map = assets.loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "basic_map.gltf");
    if (!map) {
        return {.error = map.error};
    }

    const Entity sun = scene.createEntity("Sun");
    scene.registry().emplace<DirectionalLightComponent>(sun);
    const Entity environment = scene.createEntity("Environment");
    scene.registry().emplace<EnvironmentLightComponent>(environment);

    const Entity mapRoot = scene.createEntity("Gameplay Room");
    if (const SceneResult result = scene.instantiateModel(mapRoot, map.handle, assets); !result) {
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

    const ModelAsset* model = assets.model(map.handle);
    if (!model || model->nodes.empty() || !model->nodes.front().mesh) {
        return {.error = "Gameplay room does not contain a reusable prop mesh"};
    }
    const MeshHandle cubeMesh = model->nodes.front().mesh;
    const glm::vec3 propPositions[]{
        {-2.5f, 0.7f, 1.0f}, {-2.5f, 1.55f, 1.0f}, {-2.5f, 2.4f, 1.0f},
        {2.2f, 0.7f, -1.2f}, {2.9f, 0.7f, -1.2f},  {2.55f, 1.55f, -1.2f},
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

    for (uint32_t i = 0; i < benchmarkObjectCount; ++i) {
        const Entity object = scene.createEntity("Benchmark object " + std::to_string(i + 1));
        scene.attachMesh(object, cubeMesh);
        const float x = (static_cast<float>(i % 40) - 19.5f) * 1.2f;
        const float y = (static_cast<float>((i / 40) % 10) + 0.5f) * 1.2f;
        const float z = -6.0f - static_cast<float>(i / 400) * 3.0f;
        scene.registry().get<TransformComponent>(object).local =
            glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.35f));
    }

    return {.success = true};
}
