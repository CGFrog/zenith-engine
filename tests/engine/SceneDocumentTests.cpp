#include "engine/assets/AssetDatabase.hpp"
#include "engine/scene/SceneAssetReferences.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::string serialize(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string output;
    const SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, output);
    CHECK_MESSAGE(result.success, result.error);
    return output;
}

void checkRejectedWithoutReplacement(std::string_view document, std::string_view expectedError, AssetManager& assets,
                                     const SceneDocumentOptions& options) {
    Scene scene;
    const Entity sentinel = scene.createEntity("sentinel");
    const SceneEntityId sentinelId = scene.id(sentinel);
    const SceneDocumentResult result = SceneDocument::deserialize(document, assets, options, scene);
    CHECK_MESSAGE(!result.success, "invalid document should be rejected");
    CHECK_MESSAGE(result.error.find(expectedError) != std::string::npos,
                  "load error should describe the rejected input");
    CHECK_MESSAGE((scene.findEntity(sentinelId) != entt::null), "failed load should preserve the active scene");
    CHECK_MESSAGE(scene.registry().get<NameComponent>(scene.findEntity(sentinelId)).name == "sentinel",
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

void checkPatchRejected(const SceneChanges& changes, std::string_view expectedError, Scene& scene, AssetManager& assets,
                        const SceneDocumentOptions& options) {
    const std::string before = serialize(scene, assets, options);
    const SceneDocumentResult result =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene);
    CHECK_MESSAGE(!result.success, "an invalid entity patch should be rejected");
    CHECK_MESSAGE(result.error.find(expectedError) != std::string::npos,
                  "patch error should describe the rejected input");
    CHECK_MESSAGE(serialize(scene, assets, options) == before,
                  "a rejected entity patch should leave the scene unchanged");
}

// A scene exercising the serialized components: a parented child with a
// camera, collider, and rigid body, a volumetric fog entity, and, when the
// sample model loads, an animated model instance with a skin.
struct RoundTripScene {
    explicit RoundTripScene(AssetManager& assets) {
        const Entity root = scene.createEntity("Root");
        const Entity child = scene.createEntity("Child");
        scene.setParent(child, root);
        scene.registry().get<TransformComponent>(child).local[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
        scene.registry().emplace<CameraComponent>(child);
        scene.registry().emplace<ColliderComponent>(child, SphereCollider{2.0f});
        RigidBodyComponent body;
        body.motionType = PhysicsMotionType::Dynamic;
        body.mass = 3.0f;
        scene.registry().emplace<RigidBodyComponent>(child, body);
        fog = scene.createEntity("Fog");
        scene.registry().emplace<VolumetricFogComponent>(fog, VolumetricFogComponent{.density = 0.04f,
                                                                                     .heightFalloff = 0.25f,
                                                                                     .baseHeight = -1.5f,
                                                                                     .albedo = {0.8f, 0.85f, 0.9f},
                                                                                     .anisotropy = -0.2f,
                                                                                     .noiseAmount = 0.5f,
                                                                                     .noiseScale = 6.0f,
                                                                                     .primary = false});

        const AssetLoadResult<ModelHandle> model =
            assets.loadModel(std::filesystem::path(ZENITH_ASSET_DIR) / "sample.gltf");
        CHECK_MESSAGE(static_cast<bool>(model), model.error);
        if (model) {
            const Entity modelRoot = scene.createEntity("Model");
            CHECK_MESSAGE(scene.instantiateModel(modelRoot, model.handle, assets).success,
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
    }

    Scene scene;
    Entity fog = entt::null;
};

const nlohmann::json& onlyEntityWith(const nlohmann::json& document, std::string_view component) {
    static const nlohmann::json empty = nlohmann::json::object();
    for (const nlohmann::json& entity : document.at("entities")) {
        if (entity.at("components").contains(component)) {
            return entity.at("components").at(component);
        }
    }
    return empty;
}

} // namespace

TEST_SUITE_BEGIN("SceneDocument");

TEST_CASE("SceneDocument: created entities receive unique stable ids") {
    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity child = scene.createEntity("Child");
    CHECK_MESSAGE((scene.id(root) && scene.id(child) && scene.id(root) != scene.id(child)),
                  "created entities should receive unique stable IDs");
    CHECK_MESSAGE(scene.findEntity(scene.id(child)) == child, "stable IDs should resolve to runtime entities");
}

TEST_CASE("SceneDocument: a loaded document serializes to identical content") {
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    AssetManager assets;
    const RoundTripScene fixture(assets);
    const Scene& scene = fixture.scene;

    const std::string original = serialize(scene, assets, options);
    Scene restored;
    const SceneDocumentResult restoredResult = SceneDocument::deserialize(original, assets, options, restored);
    CHECK_MESSAGE(restoredResult.success, restoredResult.error);
    CHECK_MESSAGE(serialize(restored, assets, options) == original,
                  "serializing after a load should reproduce identical document content");
    const Entity restoredFog = restored.findEntity(scene.id(fixture.fog));
    const auto* restoredMedium =
        restoredFog != entt::null ? restored.registry().try_get<VolumetricFogComponent>(restoredFog) : nullptr;
    CHECK_MESSAGE((restoredMedium != nullptr && restoredMedium->density == 0.04f &&
                   restoredMedium->baseHeight == -1.5f && restoredMedium->albedo == glm::vec3(0.8f, 0.85f, 0.9f) &&
                   restoredMedium->anisotropy == -0.2f && restoredMedium->noiseScale == 6.0f &&
                   !restoredMedium->primary),
                  "volumetric fog parameters should round trip");
}

TEST_CASE("SceneDocument: an atomic save loads back from disk") {
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    AssetManager assets;
    const RoundTripScene fixture(assets);
    const Scene& scene = fixture.scene;
    const std::string original = serialize(scene, assets, options);

    const testsupport::TempDirectory tempDirectory("scene-document-save");
    const std::filesystem::path scenePath = tempDirectory / "round_trip.zenith-scene";
    CHECK_MESSAGE(SceneDocument::save(scenePath, scene, assets, options).success, "atomic scene save should succeed");
    CHECK_MESSAGE(!std::filesystem::exists(scenePath.string() + ".tmp"),
                  "atomic save should not leave its temporary file");
    Scene loadedFromDisk;
    CHECK_MESSAGE(SceneDocument::load(scenePath, assets, options, loadedFromDisk).success,
                  "saved scene should load from disk");
    CHECK_MESSAGE(serialize(loadedFromDisk, assets, options) == original, "file round trip should preserve the scene");
}

TEST_CASE("SceneDocument: invalid documents are rejected without replacing the active scene") {
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    AssetManager assets;
    checkRejectedWithoutReplacement("not json", "Invalid scene document", assets, options);
    checkRejectedWithoutReplacement(R"({"version":99,"entities":[]})", "Unsupported scene document version", assets,
                                    options);
    checkRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{}},{"id":1,"name":"b","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{}}]})",
        "Duplicate scene entity ID", assets, options);
    checkRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":2,"components":{}},{"id":2,"name":"b","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":1,"components":{}}]})",
        "cycle", assets, options);
    checkRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":2,"components":{}}]})",
        "missing parent", assets, options);
    checkRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{"model":{"path":"missing.gltf"}}}]})",
        "Missing asset", assets, options);
}

TEST_CASE("SceneDocument: invalid volumetric fog parameters are rejected") {
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    AssetManager assets;
    checkRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{"volumetric_fog":{"density":0.1,"height_falloff":0.1,"base_height":0,"albedo":[1,1,1],"anisotropy":1.0,"primary":true}}}]})",
        "Volumetric fog parameters are invalid", assets, options);
    checkRejectedWithoutReplacement(
        R"({"version":1,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{"volumetric_fog":{"density":1e39,"height_falloff":0.1,"base_height":0,"albedo":[1,1,1],"anisotropy":0.2,"primary":true}}}]})",
        "Volumetric fog parameters are invalid", assets, options);
}

TEST_CASE("SceneDocument: entity changes") {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    // A(1) has children B(2), C(3), and G(6); D(4) is a root light carrying a
    // component this registry does not know.
    const std::string document = R"({"version":1,"entities":[)" + entityJson(1, "A", "null") + "," +
                                 entityJson(2, "B", "1") + "," + entityJson(3, "C", "1") + "," +
                                 entityJson(4, "D", "null", R"({"game_inventory":{"slots":[1,2]}})") + "," +
                                 entityJson(6, "G", "1") + "]}";
    Scene scene;
    const SceneDocumentResult loaded = SceneDocument::deserialize(document, assets, options, scene);
    CHECK_MESSAGE(loaded.success, loaded.error);
    CHECK_MESSAGE((childIds(scene, {1}) == std::vector<uint64_t>{2, 3, 6}),
                  "a document load should list children by id");
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
    CHECK_MESSAGE(scene.createEntityWithId({5}, "E", e).success, "test entity should be created");
    scene.setParent(e, b);
    const std::string after = serialize(scene, assets, options);

    SceneChanges changes;
    const SceneDocumentResult diffed = SceneDocument::diff(before, after, changes);
    CHECK_MESSAGE(diffed.success, diffed.error);
    CHECK_MESSAGE(!changes.collections, "an edit that leaves collections alone should not record them");
    std::vector<uint64_t> changedIds;
    for (const SceneEntityChange& change : changes.entities) {
        changedIds.push_back(change.id.value);
    }
    CHECK_MESSAGE((changedIds == std::vector<uint64_t>{2, 3, 4, 5}),
                  "a diff should list only added, removed, and changed ids");
    CHECK_MESSAGE((changes.entities[1].before && !changes.entities[1].after && !changes.entities[3].before &&
                   changes.entities[3].after),
                  "a diff should mark removed and added entities as absent on one side");
    std::vector<std::optional<std::string>> current;
    const std::vector<SceneEntityId> changedEntities{{2}, {3}, {4}, {5}};
    const SceneDocumentResult serializedEntities =
        SceneDocument::serializeEntities(scene, assets, options, changedEntities, current);
    CHECK_MESSAGE((serializedEntities.success && current.size() == changes.entities.size()), serializedEntities.error);
    for (size_t index = 0; index < current.size() && index < changes.entities.size(); ++index) {
        CHECK_MESSAGE(current[index] == changes.entities[index].after,
                      "serializing chosen entities should match the diff's after side byte for byte");
    }
    SceneChanges none;
    CHECK_MESSAGE((SceneDocument::diff(before, before, none).success && none.empty()),
                  "identical documents should not differ");

    const SceneDocumentResult undone =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Backward, assets, options, scene);
    CHECK_MESSAGE(undone.success, undone.error);
    CHECK_MESSAGE(serialize(scene, assets, options) == before, "patching backward should restore the before document");
    CHECK_MESSAGE((scene.findEntity({1}) == a && scene.findEntity({2}) == b && scene.findEntity({4}) == d),
                  "unchanged and rebuilt entities should keep their EnTT handles");
    CHECK_MESSAGE((childIds(scene, {1}) == std::vector<uint64_t>{2, 3, 6}),
                  "a restored entity should return to its id-ordered place among its siblings");
    CHECK_MESSAGE((scene.registry().all_of<PreservedComponentsComponent, PointLightComponent>(d)),
                  "a rebuilt entity should regain registered and preserved components");

    const SceneDocumentResult redone =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene);
    CHECK_MESSAGE(redone.success, redone.error);
    CHECK_MESSAGE(serialize(scene, assets, options) == after, "patching forward should reproduce the after document");
    CHECK_MESSAGE(!scene.registry().any_of<PointLightComponent>(d), "a rebuilt entity should lose removed components");
    CHECK_MESSAGE(
        (SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene).success &&
         serialize(scene, assets, options) == after),
        "patching a scene already in the target state should change nothing");
    CHECK_MESSAGE((childIds(scene, {1}) == std::vector<uint64_t>{2, 4, 6}), "a moved entity should be placed by id");

    const SceneChanges missingParent{.entities = {{.id = {7}, .after = entityJson(7, "H", "99")}}};
    checkPatchRejected(missingParent, "missing parent", scene, assets, options);
    const SceneChanges cycle{.entities = {{.id = {1}, .after = entityJson(1, "A", "2")}}};
    checkPatchRejected(cycle, "cycle", scene, assets, options);
    const SceneChanges orphan{.entities = {{.id = {2}, .before = entityJson(2, "B", "1")}}};
    checkPatchRejected(orphan, "unchanged child", scene, assets, options);
    const SceneChanges wrongId{.entities = {{.id = {2}, .after = entityJson(3, "B", "1")}}};
    checkPatchRejected(wrongId, "different entity", scene, assets, options);
    const SceneChanges twice{
        .entities = {{.id = {6}, .after = entityJson(6, "G", "1")}, {.id = {6}, .after = entityJson(6, "G", "null")}}};
    checkPatchRejected(twice, "twice", scene, assets, options);
    const SceneChanges invalidComponent{
        .entities = {{.id = {6}, .after = entityJson(6, "G", "1", R"({"volumetric_fog":{"density":-1}})")},
                     {.id = {8}, .after = entityJson(8, "I", "null")}}};
    checkPatchRejected(invalidComponent, "Entity 6", scene, assets, options);
    CHECK_MESSAGE((scene.findEntity({8}) == entt::null), "a rejected patch should not create any of its entities");
}

// Sibling order: a parent whose children are not in id order saves a
// "children" order; loads, patches, and new entities honor it, and documents
// without one keep listing children by id.
TEST_CASE("SceneDocument: child order") {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    Scene scene;
    const Entity parent = scene.createEntity("Parent");
    std::vector<Entity> children;
    for (const char* name : {"A", "B", "C"}) {
        children.push_back(scene.createEntity(name));
        scene.setParent(children.back(), parent);
    }
    const SceneEntityId parentId = scene.id(parent);
    const std::string idOrdered = serialize(scene, assets, options);
    CHECK_MESSAGE(idOrdered.find("\"children\"") == std::string::npos, "children in id order should not save an order");

    CHECK_MESSAGE(!scene.setSiblingIndex(parent, 0).success, "a root entity should have no sibling order");
    CHECK_MESSAGE(scene.setSiblingIndex(children[2], 0).success, "a child should move to a sibling index");
    CHECK_MESSAGE((childIds(scene, parentId) == std::vector<uint64_t>{4, 2, 3}), "the moved child should come first");
    CHECK_MESSAGE(scene.setSiblingIndex(children[0], 99).success, "an index past the end should clamp");
    CHECK_MESSAGE((childIds(scene, parentId) == std::vector<uint64_t>{4, 3, 2}), "the clamped child should come last");
    const std::string reordered = serialize(scene, assets, options);
    const nlohmann::json document = nlohmann::json::parse(reordered);
    CHECK_MESSAGE(document["entities"][0]["children"] == nlohmann::json::array({4, 3, 2}),
                  "a reordered parent should save its children's order");

    Scene loaded;
    const SceneDocumentResult load = SceneDocument::deserialize(reordered, assets, options, loaded);
    CHECK_MESSAGE(load.success, load.error);
    CHECK_MESSAGE((childIds(loaded, parentId) == std::vector<uint64_t>{4, 3, 2}),
                  "loading should restore the sibling order");
    CHECK_MESSAGE(serialize(loaded, assets, options) == reordered, "the order should round trip");

    SceneChanges changes;
    CHECK_MESSAGE((SceneDocument::diff(idOrdered, reordered, changes).success && changes.entities.size() == 1 &&
                   changes.entities[0].id == parentId),
                  "reordering should change only the parent's document form");
    const SceneDocumentResult backward =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Backward, assets, options, scene);
    CHECK_MESSAGE(backward.success, backward.error);
    CHECK_MESSAGE((childIds(scene, parentId) == std::vector<uint64_t>{2, 3, 4}),
                  "patching back should restore id order");
    const SceneDocumentResult forward =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene);
    CHECK_MESSAGE(forward.success, forward.error);
    CHECK_MESSAGE((childIds(scene, parentId) == std::vector<uint64_t>{4, 3, 2}),
                  "patching forward should reorder again");

    // A new child of an ordered parent is listed after the ordered ones; an
    // order naming an entity that is not a child is ignored.
    Scene extended;
    const std::string stale = entityJson(1, "Parent", "null").insert(1, R"("children":[9,3,2],)");
    const std::string withStale = R"({"version":3,"collections":[],"entities":[)" + stale + "," +
                                  entityJson(2, "A", "1") + "," + entityJson(3, "B", "1") + "," +
                                  entityJson(4, "C", "1") + "]}";
    const SceneDocumentResult staleLoad = SceneDocument::deserialize(withStale, assets, options, extended);
    CHECK_MESSAGE(staleLoad.success, staleLoad.error);
    CHECK_MESSAGE((childIds(extended, parentId) == std::vector<uint64_t>{3, 2, 4}),
                  "unlisted children should follow the listed ones in id order");
    const nlohmann::json created =
        nlohmann::json::parse(entityJson(10, "D", "null").insert(1, R"("children":[12,11],)"));
    const nlohmann::json first = nlohmann::json::parse(entityJson(11, "E", "10"));
    const nlohmann::json second = nlohmann::json::parse(entityJson(12, "F", "10"));
    const std::vector<SceneEntityDocument> added{{{10}, &created}, {{11}, &first}, {{12}, &second}};
    const SceneDocumentResult creation = SceneDocument::createEntities(added, assets, options, extended);
    CHECK_MESSAGE(creation.success, creation.error);
    CHECK_MESSAGE((childIds(extended, {10}) == std::vector<uint64_t>{12, 11}),
                  "created entities should honor their order");

    const std::string invalid = R"({"version":3,"collections":[],"entities":[)" +
                                entityJson(1, "Parent", "null").insert(1, R"("children":"no",)") + "]}";
    checkRejectedWithoutReplacement(invalid, "children order", assets, options);
}

// Version 1 documents migrate on load, references carry GUIDs in version 2,
// and a reference whose asset moved resolves through the asset database.
TEST_CASE("SceneDocument: asset references") {
    const std::filesystem::path sourceAssets = ZENITH_ASSET_DIR;
    const testsupport::TempDirectory references("scene-document-references");
    const std::filesystem::path root = references.path();
    std::filesystem::create_directories(root / "models");
    std::filesystem::create_directories(root / "textures");
    std::filesystem::copy_file(sourceAssets / "primitives" / "cube.gltf", root / "models" / "cube.gltf");
    std::filesystem::copy_file(sourceAssets / "showcase" / "decals" / "decal_arrow.png",
                               root / "textures" / "arrow.png");

    const std::string version1 =
        R"({"version":1,"entities":[)" + entityJson(1, "Cube", "null", R"({"model":{"path":"models/cube.gltf"}})") +
        "," +
        entityJson(2, "Decal", "null",
                   R"({"decal":{"half_extents":[1,1,1],"albedo_asset":"textures/arrow.png","normal_asset":""}})") +
        "]}";
    const SceneDocumentOptions pathOnly{.assetRoot = root};
    AssetManager assets;
    Scene migrated;
    const SceneDocumentResult migratedResult = SceneDocument::deserialize(version1, assets, pathOnly, migrated);
    CHECK_MESSAGE(migratedResult.success,
                  ("a version 1 document should load through migration: " + migratedResult.error));
    const auto* decal = migrated.registry().try_get<DecalComponent>(migrated.findEntity({2}));
    CHECK_MESSAGE((decal != nullptr && decal->albedoAsset == "textures/arrow.png" && decal->albedoTexture &&
                   decal->normalAsset.empty()),
                  "migrated texture references should load their textures");

    const nlohmann::json pathOnlyDocument = nlohmann::json::parse(serialize(migrated, assets, pathOnly));
    CHECK_MESSAGE(pathOnlyDocument.at("version") == SceneDocument::currentVersion,
                  "saving should write the current version");
    CHECK_MESSAGE((onlyEntityWith(pathOnlyDocument, "decal").at("albedo_asset") ==
                   nlohmann::json{{"path", "textures/arrow.png"}}),
                  "without a database a reference should be a path-only object");
    CHECK_MESSAGE(onlyEntityWith(pathOnlyDocument, "decal").at("normal_asset") == "",
                  "an empty reference should stay an empty string");

    const auto database = std::make_shared<AssetDatabase>(root, AssetDatabaseOptions{.guidSeed = 7});
    CHECK_MESSAGE(database->rescan().success, "the reference test database should scan");
    const SceneDocumentOptions withDatabase{.assetRoot = root, .assetDatabase = database};
    const std::string saved = serialize(migrated, assets, withDatabase);
    const nlohmann::json savedDocument = nlohmann::json::parse(saved);
    const std::string modelGuid = database->guidForPath("models/cube.gltf").value_or(AssetGuid{}).toString();
    const std::string textureGuid = database->guidForPath("textures/arrow.png").value_or(AssetGuid{}).toString();
    CHECK_MESSAGE((onlyEntityWith(savedDocument, "model").value("guid", "") == modelGuid &&
                   onlyEntityWith(savedDocument, "model").value("path", "") == "models/cube.gltf"),
                  "model references should carry the asset GUID beside the path");
    CHECK_MESSAGE((onlyEntityWith(savedDocument, "decal").at("albedo_asset") ==
                   nlohmann::json{{"guid", textureGuid}, {"path", "textures/arrow.png"}}),
                  "texture references should carry the asset GUID beside the path");

    // The assets move; the saved document still names the old paths.
    CHECK_MESSAGE((database->move("models/cube.gltf", "props/box.gltf").success &&
                   database->move("textures/arrow.png", "textures/signs/arrow.png").success),
                  "the database should move the referenced assets");
    Scene moved;
    const SceneDocumentResult movedResult = SceneDocument::deserialize(saved, assets, withDatabase, moved);
    CHECK_MESSAGE(movedResult.success, ("references to moved assets should resolve by GUID: " + movedResult.error));
    const nlohmann::json resaved = nlohmann::json::parse(serialize(moved, assets, withDatabase));
    CHECK_MESSAGE((onlyEntityWith(resaved, "model").value("path", "") == "props/box.gltf" &&
                   onlyEntityWith(resaved, "decal").at("albedo_asset").value("path", "") == "textures/signs/arrow.png"),
                  "resaving should write the moved assets' new paths");
    Scene withoutDatabase;
    CHECK_MESSAGE(!SceneDocument::deserialize(saved, assets, pathOnly, withoutDatabase).success,
                  "without a database a moved asset cannot be found by its old path");

    const std::map<std::string, std::string> movedPaths{{"props/box.gltf", "props/crate.gltf"}};
    std::string relocatedDocument;
    size_t relocated = 0;
    CHECK_MESSAGE(
        (relocateSceneAssetReferences(serialize(moved, assets, withDatabase), movedPaths, relocatedDocument, relocated)
             .success &&
         relocated == 1 && relocatedDocument.find("props/crate.gltf") != std::string::npos &&
         relocatedDocument.find("props/box.gltf") == std::string::npos),
        "relocation should rewrite every reference to a moved path");

    Scene rejected;
    std::string future = version1;
    future.replace(future.find(R"("version":1)"), 11, R"("version":4)");
    const SceneDocumentResult futureResult = SceneDocument::deserialize(future, assets, pathOnly, rejected);
    CHECK_MESSAGE((!futureResult.success && futureResult.error.find("newer") != std::string::npos),
                  "documents from a newer format version should be rejected with a clear message");
    std::string unknownGuid = saved;
    unknownGuid.replace(unknownGuid.find(textureGuid), textureGuid.size(), std::string(32, 'f'));
    const SceneDocumentResult unknownResult = SceneDocument::deserialize(unknownGuid, assets, withDatabase, rejected);
    CHECK_MESSAGE((!unknownResult.success && unknownResult.error.find("Missing asset") != std::string::npos),
                  "an unknown GUID should fall back to the stored path, which has moved away");
}

TEST_SUITE_END();
