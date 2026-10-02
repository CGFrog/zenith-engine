#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

std::string entityJson(uint64_t id, std::string_view name, std::string_view parent,
                       std::string_view components = "{}") {
    return R"({"id":)" + std::to_string(id) + R"(,"name":")" + std::string(name) +
           R"(","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":)" + std::string(parent) +
           R"(,"components":)" + std::string(components) + "}";
}

std::vector<uint64_t> childIds(const Scene& scene, SceneEntityId parent) {
    std::vector<uint64_t> ids;
    for (const Entity child : scene.registry().get<RelationshipComponent>(scene.findEntity(parent)).children) {
        ids.push_back(scene.id(child).value);
    }
    return ids;
}

void expectPatchRejected(std::span<const SceneEntityChange> changes, std::string_view expectedError, Scene& scene,
                         AssetManager& assets, const SceneDocumentOptions& options) {
    const std::string before = serialize(scene, assets, options);
    const SceneDocumentResult result =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene);
    expect(!result.success, "an invalid entity patch should be rejected");
    expect(result.error.find(expectedError) != std::string::npos, "patch error should describe the rejected input");
    expect(serialize(scene, assets, options) == before, "a rejected entity patch should leave the scene unchanged");
}

void testEntityChanges(AssetManager& assets, const SceneDocumentOptions& options) {
    // A(1) has children B(2), C(3), and G(6); D(4) is a root light carrying a
    // component this registry does not know.
    const std::string document = R"({"version":1,"entities":[)" + entityJson(1, "A", "null") + "," +
                                 entityJson(2, "B", "1") + "," + entityJson(3, "C", "1") + "," +
                                 entityJson(4, "D", "null", R"({"game_inventory":{"slots":[1,2]}})") + "," +
                                 entityJson(6, "G", "1") + "]}";
    Scene scene;
    const SceneDocumentResult loaded = SceneDocument::deserialize(document, assets, options, scene);
    expect(loaded.success, loaded.error);
    expect(childIds(scene, {1}) == std::vector<uint64_t>{2, 3, 6}, "a document load should list children by id");
    scene.registry().emplace<PointLightComponent>(scene.findEntity({4}), glm::vec3(1.0f), 2.0f, 3.0f);
    const std::string before = serialize(scene, assets, options);
    const Entity a = scene.findEntity({1});
    const Entity b = scene.findEntity({2});
    const Entity d = scene.findEntity({4});

    scene.registry().get<NameComponent>(b).name = "B renamed";
    scene.destroyEntity(scene.findEntity({3}));
    scene.setParent(d, a);
    scene.registry().remove<PointLightComponent>(d);
    Entity e = entt::null;
    expect(scene.createEntityWithId({5}, "E", e).success, "test entity should be created");
    scene.setParent(e, b);
    const std::string after = serialize(scene, assets, options);

    std::vector<SceneEntityChange> changes;
    const SceneDocumentResult diffed = SceneDocument::diff(before, after, changes);
    expect(diffed.success, diffed.error);
    std::vector<uint64_t> changedIds;
    for (const SceneEntityChange& change : changes) {
        changedIds.push_back(change.id.value);
    }
    expect(changedIds == std::vector<uint64_t>{2, 3, 4, 5}, "a diff should list only added, removed, and changed ids");
    expect(changes[1].before && !changes[1].after && !changes[3].before && changes[3].after,
           "a diff should mark removed and added entities as absent on one side");
    std::vector<SceneEntityChange> none;
    expect(SceneDocument::diff(before, before, none).success && none.empty(), "identical documents should not differ");

    const SceneDocumentResult undone =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Backward, assets, options, scene);
    expect(undone.success, undone.error);
    expect(serialize(scene, assets, options) == before, "patching backward should restore the before document");
    expect(scene.findEntity({1}) == a && scene.findEntity({2}) == b && scene.findEntity({4}) == d,
           "unchanged and rebuilt entities should keep their EnTT handles");
    expect(childIds(scene, {1}) == std::vector<uint64_t>{2, 3, 6},
           "a restored entity should return to its id-ordered place among its siblings");
    expect(scene.registry().all_of<PreservedComponentsComponent, PointLightComponent>(d),
           "a rebuilt entity should regain registered and preserved components");

    const SceneDocumentResult redone =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene);
    expect(redone.success, redone.error);
    expect(serialize(scene, assets, options) == after, "patching forward should reproduce the after document");
    expect(!scene.registry().any_of<PointLightComponent>(d), "a rebuilt entity should lose removed components");
    expect(SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene).success &&
               serialize(scene, assets, options) == after,
           "patching a scene already in the target state should change nothing");
    expect(childIds(scene, {1}) == std::vector<uint64_t>{2, 4, 6}, "a moved entity should be placed by id");

    const std::vector<SceneEntityChange> missingParent{{.id = {7}, .after = entityJson(7, "H", "99")}};
    expectPatchRejected(missingParent, "missing parent", scene, assets, options);
    const std::vector<SceneEntityChange> cycle{{.id = {1}, .after = entityJson(1, "A", "2")}};
    expectPatchRejected(cycle, "cycle", scene, assets, options);
    const std::vector<SceneEntityChange> orphan{{.id = {2}, .before = entityJson(2, "B", "1")}};
    expectPatchRejected(orphan, "unchanged child", scene, assets, options);
    const std::vector<SceneEntityChange> wrongId{{.id = {2}, .after = entityJson(3, "B", "1")}};
    expectPatchRejected(wrongId, "different entity", scene, assets, options);
    const std::vector<SceneEntityChange> twice{{.id = {6}, .after = entityJson(6, "G", "1")},
                                               {.id = {6}, .after = entityJson(6, "G", "null")}};
    expectPatchRejected(twice, "twice", scene, assets, options);
    const std::vector<SceneEntityChange> invalidComponent{
        {.id = {6}, .after = entityJson(6, "G", "1", R"({"volumetric_fog":{"density":-1}})")},
        {.id = {8}, .after = entityJson(8, "I", "null")}};
    expectPatchRejected(invalidComponent, "Entity 6", scene, assets, options);
    expect(scene.findEntity({8}) == entt::null, "a rejected patch should not create any of its entities");
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

    testEntityChanges(assets, options);

    std::filesystem::remove_all(tempDirectory, filesystemError);
    return failures == 0 ? 0 : 1;
}
