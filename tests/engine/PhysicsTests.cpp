#include "engine/physics/PhysicsWorld.hpp"

#include <cmath>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics world initializes");

    Scene scene;
    AssetManager assets;
    const Entity floor = scene.createEntity("Floor");
    scene.registry().get<TransformComponent>(floor).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(5.0f, 0.5f, 5.0f));
    scene.registry().emplace<ColliderComponent>(floor, BoxCollider{glm::vec3(1.0f)});
    scene.registry().emplace<RigidBodyComponent>(floor);

    const Entity box = scene.createEntity("Box");
    scene.registry().get<TransformComponent>(box).local = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f));
    scene.registry().emplace<ColliderComponent>(box, BoxCollider{glm::vec3(0.5f)});
    RigidBodyComponent body;
    body.motionType = PhysicsMotionType::Dynamic;
    body.layer = PhysicsLayer::Dynamic;
    scene.registry().emplace<RigidBodyComponent>(box, body);

    bool sawContact = false;
    for (int i = 0; i < 180; ++i) {
        const PhysicsResult result = physics.step(scene, assets, physics.config().fixedTimeStep);
        expect(static_cast<bool>(result), "physics step succeeds");
        if (!result) {
            break;
        }
        sawContact |= !physics.contactEvents().empty();
    }
    const float settledHeight = scene.registry().get<TransformComponent>(box).local[3].y;
    expect(settledHeight > 0.45f && settledHeight < 0.65f, "dynamic box settles on the floor");
    expect(sawContact, "body contacts are exposed to gameplay code");

    PhysicsRayHit hit;
    expect(physics.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit), "raycast hits a physics body");
    expect(hit.entity == box || hit.entity == floor, "raycast resolves the hit to an ECS entity");

    expect(physics.applyImpulse(box, {2.0f, 3.0f, 0.0f}), "impulse applies to a managed body");
    expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)),
           "physics advances after an impulse");
    expect(physics.linearVelocity(box).x > 0.0f, "impulse changes linear velocity");

    const auto sample = assets.loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf");
    expect(static_cast<bool>(sample), "mesh collider fixture loads");
    const ModelAsset* model = sample ? assets.model(sample.handle) : nullptr;
    if (model && !model->nodes.empty()) {
        const Entity meshEntity = scene.createEntity("Static Mesh");
        scene.registry().get<TransformComponent>(meshEntity).local =
            glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.0f, 3.0f));
        scene.registry().emplace<ColliderComponent>(meshEntity, TriangleMeshCollider{model->nodes.front().mesh});
        scene.registry().emplace<RigidBodyComponent>(meshEntity);
        expect(static_cast<bool>(physics.refreshBody(meshEntity, scene, assets)), "static triangle mesh body builds");
        PhysicsRayHit meshHit;
        expect(physics.raycast({3.0f, 4.0f, 3.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, meshHit) && meshHit.entity == meshEntity,
               "raycast hits a static triangle mesh");
    }

    scene.destroyEntity(box);
    expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)),
           "destroyed entity is removed from physics");
    physics.clear();
    PhysicsRayHit clearedHit;
    expect(!physics.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, clearedHit),
           "clearing physics removes all managed bodies");
    physics.shutdown();
    return failures == 0 ? 0 : 1;
}
