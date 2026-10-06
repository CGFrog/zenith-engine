#include "DefaultScene.hpp"

#include "engine/assets/Assets.hpp"

Entity addSun(Scene& scene) {
    const Entity sun = scene.createEntity("Sun");
    scene.registry().emplace<DirectionalLightComponent>(sun);
    return sun;
}

Entity addEnvironment(Scene& scene) {
    const Entity environment = scene.createEntity("Environment");
    scene.registry().emplace<EnvironmentLightComponent>(environment);
    return environment;
}

Entity addFog(Scene& scene) {
    const Entity fog = scene.createEntity("Volumetric Fog");
    scene.registry().emplace<VolumetricFogComponent>(fog);
    return fog;
}

Entity addCamera(Scene& scene) {
    bool primaryExists = false;
    for (const auto [entity, camera] : scene.registry().view<const CameraComponent>().each()) {
        primaryExists = primaryExists || camera.primary;
    }
    const Entity camera = scene.createEntity("Camera");
    scene.registry().emplace<CameraComponent>(camera).primary = !primaryExists;
    return camera;
}

Entity addEmpty(Scene& scene) {
    return scene.createEntity("Empty");
}

Entity addCollider(Scene& scene, const ColliderShape& shape, PhysicsMotionType motion) {
    const char* name = "Box Collider";
    if (std::holds_alternative<SphereCollider>(shape)) {
        name = "Sphere Collider";
    } else if (std::holds_alternative<CapsuleCollider>(shape)) {
        name = "Capsule Collider";
    } else if (std::holds_alternative<TriangleMeshCollider>(shape)) {
        name = "Mesh Collider";
    } else if (std::holds_alternative<ConvexHullCollider>(shape)) {
        name = "Convex Hull Collider";
    }
    const Entity entity = scene.createEntity(name);
    scene.registry().emplace<ColliderComponent>(entity, ColliderComponent{.shape = shape});
    scene.registry().emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = motion,
                                                                            .layer = motion == PhysicsMotionType::Static
                                                                                         ? PhysicsLayer::Static
                                                                                         : PhysicsLayer::Dynamic});
    return entity;
}

SceneResult addCube(Scene& scene, AssetManager& assets, const std::filesystem::path& assetRoot, Entity& cube) {
    const std::filesystem::path cubePath = assetRoot / defaultCubeModel;
    const auto model = assets.loadModel(cubePath);
    if (!model) {
        return {.error = "Default cube unavailable: " + model.error};
    }
    const ModelAsset* asset = assets.model(model.handle);
    if (asset == nullptr || asset->nodes.empty() || !asset->nodes.front().mesh) {
        return {.error = "Default cube model has no mesh: " + cubePath.string()};
    }
    cube = scene.createEntity("Cube");
    scene.attachMesh(cube, asset->nodes.front().mesh);
    return {.success = true};
}

SceneResult populateDefaultScene(Scene& scene, AssetManager& assets, const std::filesystem::path& assetRoot) {
    addSun(scene);
    addEnvironment(scene);
    Entity cube = entt::null;
    return addCube(scene, assets, assetRoot, cube);
}
