#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::string serialize(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string output;
    const SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, output);
    expect(result.success, result.error);
    return output;
}

void expectRejectedWithoutReplacement(std::string_view document, std::string_view expectedError, AssetManager& assets,
                                      const SceneDocumentOptions& options) {
    Scene scene;
    const Entity sentinel = scene.createEntity("sentinel");
    const SceneEntityId sentinelId = scene.id(sentinel);
    const SceneDocumentResult result = SceneDocument::deserialize(document, assets, options, scene);
    expect(!result.success, "invalid document should be rejected");
    expect(result.error.find(expectedError) != std::string::npos, "load error should describe the rejected input");
    expect(scene.findEntity(sentinelId) != entt::null, "failed load should preserve the active scene");
    expect(scene.registry().get<NameComponent>(scene.findEntity(sentinelId)).name == "sentinel",
           "failed load should preserve active scene components");
}
} // namespace

int main() {
    const std::filesystem::path assetRoot = ZENITH_ASSET_DIR;
    const SceneDocumentOptions options{.assetRoot = assetRoot};
    AssetManager assets;

    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity child = scene.createEntity("Child");
    expect(scene.id(root) && scene.id(child) && scene.id(root) != scene.id(child),
           "created entities should receive unique stable IDs");
    expect(scene.findEntity(scene.id(child)) == child, "stable IDs should resolve to runtime entities");
    scene.setParent(child, root);
    scene.registry().get<TransformComponent>(child).local[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
    scene.registry().emplace<CameraComponent>(child);
    scene.registry().emplace<ColliderComponent>(child, SphereCollider{2.0f});
    RigidBodyComponent body;
    body.motionType = PhysicsMotionType::Dynamic;
    body.mass = 3.0f;
    scene.registry().emplace<RigidBodyComponent>(child, body);
    const Entity fog = scene.createEntity("Fog");
    scene.registry().emplace<VolumetricFogComponent>(fog, VolumetricFogComponent{.density = 0.04f,
                                                                                 .heightFalloff = 0.25f,
                                                                                 .baseHeight = -1.5f,
                                                                                 .albedo = {0.8f, 0.85f, 0.9f},
                                                                                 .anisotropy = -0.2f,
                                                                                 .noiseAmount = 0.5f,
                                                                                 .noiseScale = 6.0f,
                                                                                 .primary = false});

    const AssetLoadResult<ModelHandle> model = assets.loadModel(assetRoot / "sample.gltf");
    expect(static_cast<bool>(model), model.error);
    if (model) {
        const Entity modelRoot = scene.createEntity("Model");
        expect(scene.instantiateModel(modelRoot, model.handle, assets).success,
               "model should instantiate for round trip");
        AnimationComponent animation;
        animation.clipIndex = 2;
        animation.time = 0.75f;
        animation.playbackSpeed = 1.5f;
        animation.looping = false;
        animation.playing = false;
        animation.blendClipIndex = 3;
        animation.blendTime = 0.2f;
        animation.blendDuration = 0.5f;
        scene.registry().emplace<AnimationComponent>(modelRoot, animation);
        const auto& modelChildren = scene.registry().get<RelationshipComponent>(modelRoot).children;
        if (!modelChildren.empty()) {
            scene.registry().emplace<SkinComponent>(modelChildren.front(), model.handle, scene.id(modelRoot), 0u);
        }
    }

    const std::string original = serialize(scene, assets, options);
    Scene restored;
    const SceneDocumentResult restoredResult = SceneDocument::deserialize(original, assets, options, restored);
    expect(restoredResult.success, restoredResult.error);
    expect(serialize(restored, assets, options) == original,
           "serializing after a load should reproduce identical document content");
    const Entity restoredFog = restored.findEntity(scene.id(fog));
    const auto* restoredMedium =
        restoredFog != entt::null ? restored.registry().try_get<VolumetricFogComponent>(restoredFog) : nullptr;
    expect(restoredMedium != nullptr && restoredMedium->density == 0.04f && restoredMedium->baseHeight == -1.5f &&
               restoredMedium->albedo == glm::vec3(0.8f, 0.85f, 0.9f) && restoredMedium->anisotropy == -0.2f &&
               restoredMedium->noiseScale == 6.0f && !restoredMedium->primary,
           "volumetric fog parameters should round trip");

    const std::filesystem::path tempDirectory = std::filesystem::temp_directory_path() / "zenith_scene_document_tests";
    std::error_code filesystemError;
    std::filesystem::remove_all(tempDirectory, filesystemError);
    const std::filesystem::path scenePath = tempDirectory / "round_trip.zenith-scene";
    expect(SceneDocument::save(scenePath, scene, assets, options).success, "atomic scene save should succeed");
    expect(!std::filesystem::exists(scenePath.string() + ".tmp"), "atomic save should not leave its temporary file");
    Scene loadedFromDisk;
    expect(SceneDocument::load(scenePath, assets, options, loadedFromDisk).success,
           "saved scene should load from disk");
    expect(serialize(loadedFromDisk, assets, options) == original, "file round trip should preserve the scene");

    expectRejectedWithoutReplacement("not json", "Invalid scene document", assets, options);
    expectRejectedWithoutReplacement(R"({"version":99,"entities":[]})", "Unsupported scene document version", assets,
                                     options);
    expectRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{}},{"id":1,"name":"b","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{}}]})",
        "Duplicate scene entity ID", assets, options);
    expectRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":2,"components":{}},{"id":2,"name":"b","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":1,"components":{}}]})",
        "cycle", assets, options);
    expectRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":2,"components":{}}]})",
        "missing parent", assets, options);
    expectRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{"model":{"path":"missing.gltf"}}}]})",
        "Missing asset", assets, options);

    expectRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{"volumetric_fog":{"density":0.1,"height_falloff":0.1,"base_height":0,"albedo":[1,1,1],"anisotropy":1.0,"primary":true}}}]})",
        "Volumetric fog parameters are invalid", assets, options);
    expectRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{"volumetric_fog":{"density":1e39,"height_falloff":0.1,"base_height":0,"albedo":[1,1,1],"anisotropy":0.2,"primary":true}}}]})",
        "Volumetric fog parameters are invalid", assets, options);

    std::filesystem::remove_all(tempDirectory, filesystemError);
    return failures == 0 ? 0 : 1;
}
