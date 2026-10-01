#include "DemoScene.hpp"

#include "engine/resources/ResourceLocator.hpp"

#include "engine/assets/Assets.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <string>

SceneResult populateDemoScene(Scene& scene, AssetManager& assets, uint32_t benchmarkObjectCount,
                              uint32_t benchmarkLightCount) {
    const auto map = assets.loadModel(assetRoot() / "basic_map.gltf");
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

    // Twelve columns, widening to a square grid for large light counts.
    const uint32_t lightColumns =
        std::max(12u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(benchmarkLightCount)))));
    for (uint32_t index = 0; index < benchmarkLightCount; ++index) {
        const Entity light = scene.createEntity("Benchmark local light " + std::to_string(index + 1));
        const float x = (static_cast<float>(index % lightColumns) - (lightColumns - 1) * 0.5f) * 3.0f;
        const float z = -3.0f - static_cast<float>(index / lightColumns) * 3.0f;
        scene.registry().get<TransformComponent>(light).local = glm::translate(glm::mat4(1.0f), glm::vec3(x, 2.5f, z));
        const glm::vec3 color = index % 3 == 0   ? glm::vec3(1.0f, 0.25f, 0.12f)
                                : index % 3 == 1 ? glm::vec3(0.15f, 0.8f, 0.3f)
                                                 : glm::vec3(0.18f, 0.35f, 1.0f);
        if (index % 4 == 0) {
            SpotLightComponent spot;
            spot.color = color;
            spot.intensity = 30.0f;
            spot.range = 8.0f;
            scene.registry().emplace<SpotLightComponent>(light, spot);
        } else {
            scene.registry().emplace<PointLightComponent>(light, color, 20.0f, 7.0f);
        }
    }

    return {.success = true};
}
