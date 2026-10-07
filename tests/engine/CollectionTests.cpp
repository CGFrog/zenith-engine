// Collections, visibility flags, collection instancing, and their scene
// document form (format version 3), and the scene lighting that follows
// visibility. CPU only.

#include "engine/assets/Assets.hpp"
#include "engine/graphics/Lighting.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_BEGIN("Collection");

namespace {
std::string serialize(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string output;
    const SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, output);
    CHECK_MESSAGE(result.success, (result.error));
    return output;
}

SceneCollectionId create(SceneCollections& collections, std::string name, SceneCollectionId parent = {}) {
    SceneCollectionId id;
    CHECK_MESSAGE(collections.create(std::move(name), parent, id).success, "a collection should be created");
    return id;
}

Entity entityAt(Scene& scene, std::string name, glm::vec3 position) {
    const Entity entity = scene.createEntity(std::move(name));
    scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
    scene.markTransformDirty(entity);
    return entity;
}

} // namespace

TEST_CASE("Collection: collection definitions") {
    SceneCollections collections;
    const SceneCollectionId props = create(collections, "Props");
    const SceneCollectionId crates = create(collections, "Crates", props);
    const SceneCollectionId lights = create(collections, "Lights");
    CHECK_MESSAGE((props.value == 1 && crates.value == 2 && lights.value == 3), "collection ids should be sequential");
    CHECK_MESSAGE((collections.subtree(props) == std::vector<SceneCollectionId>{props, crates}),
                  "a subtree should list a collection and its nested collections");
    CHECK_MESSAGE((collections.setFlag(props, CollectionFlag::Visible, false).success &&
                   !collections.effectiveFlag(crates, CollectionFlag::Visible) &&
                   collections.effectiveFlag(crates, CollectionFlag::Renderable)),
                  "hiding a collection should hide the collections nested in it");
    CHECK_MESSAGE(!collections.setParent(props, crates).success, "a collection must not be nested inside itself");
    CHECK_MESSAGE(!collections.add({.id = crates, .name = "Duplicate"}).success, "duplicate ids should be rejected");
    CHECK_MESSAGE(!collections.add({.id = {9}, .name = "Orphan", .parent = {42}}).success,
                  "a collection with a missing parent should be rejected");
    CHECK_MESSAGE((collections.remove(props).success && collections.find(crates)->parent == SceneCollectionId{}),
                  "removing a collection should move its nested collections to its parent");
    SceneCollectionId next;
    CHECK_MESSAGE((collections.create("Next", {}, next).success && next.value == 4),
                  "ids should not be reused after a removal");
}

TEST_CASE("Collection: visibility rules") {
    Scene scene;
    SceneCollections collections;
    const SceneCollectionId shown = create(collections, "Shown");
    const SceneCollectionId hiddenParent = create(collections, "Hidden parent");
    const SceneCollectionId hiddenChild = create(collections, "Hidden child", hiddenParent);
    collections.setFlag(hiddenParent, CollectionFlag::Visible, false);
    const SceneCollectionId unselectable = create(collections, "Unselectable");
    collections.setFlag(unselectable, CollectionFlag::Selectable, false);
    const SceneCollectionId notRendered = create(collections, "Not rendered");
    collections.setFlag(notRendered, CollectionFlag::Renderable, false);
    scene.setCollections(collections);

    const Entity loose = scene.createEntity("Loose");
    const Entity inHidden = scene.createEntity("In hidden");
    const Entity inBoth = scene.createEntity("In hidden and shown");
    const Entity locked = scene.createEntity("Locked");
    const Entity excluded = scene.createEntity("Excluded");
    const Entity hiddenEntity = scene.createEntity("Hidden entity");
    const Entity unrenderable = scene.createEntity("Unrenderable");
    CHECK_MESSAGE(linkToCollection(scene, inHidden, hiddenChild).success, "linking should succeed");
    linkToCollection(scene, inBoth, hiddenChild);
    linkToCollection(scene, inBoth, shown);
    linkToCollection(scene, locked, unselectable);
    linkToCollection(scene, excluded, notRendered);
    scene.registry().emplace<VisibilityComponent>(hiddenEntity, VisibilityComponent{.hidden = true});
    scene.registry().emplace<VisibilityComponent>(unrenderable, VisibilityComponent{.renderable = false});
    CHECK_MESSAGE(!linkToCollection(scene, loose, {77}).success, "linking to a missing collection should fail");

    const SceneVisibility editor(scene, SceneVisibilityMode::Editor);
    const SceneVisibility runtime(scene, SceneVisibilityMode::Runtime);
    CHECK_MESSAGE((editor.drawn(loose) && editor.selectable(loose)), "entities outside collections should show");
    CHECK_MESSAGE((!editor.drawn(inHidden) && runtime.drawn(inHidden)),
                  "a hidden enclosing collection should hide members in the editor only");
    CHECK_MESSAGE(editor.drawn(inBoth), "an entity in any visible collection should stay visible");
    CHECK_MESSAGE((editor.drawn(locked) && !editor.selectable(locked)),
                  "unselectable collections should block picking only");
    CHECK_MESSAGE((!editor.drawn(excluded) && !runtime.drawn(excluded)),
                  "non-renderable collections should never draw");
    CHECK_MESSAGE((!editor.drawn(hiddenEntity) && !editor.selectable(hiddenEntity) && runtime.drawn(hiddenEntity)),
                  "the entity viewport hide should apply to editor views only");
    CHECK_MESSAGE((!editor.drawn(unrenderable) && !runtime.drawn(unrenderable)),
                  "non-renderable entities should never draw");
    CHECK_MESSAGE((editor.drawnInInstance(inHidden) && !editor.drawnInInstance(excluded)),
                  "instances should ignore the viewport hide but honour renderable");

    CHECK_MESSAGE((unlinkFromCollection(scene, locked, unselectable).success &&
                   !scene.registry().any_of<CollectionMembershipComponent>(locked)),
                  "unlinking the last collection should drop the membership component");
    const uint64_t renderableRevision = scene.revisions().renderable;
    CHECK_MESSAGE(deleteCollection(scene, hiddenChild).success, "deleting a collection should succeed");
    CHECK_MESSAGE(scene.revisions().renderable != renderableRevision,
                  "deleting a collection should invalidate renders");
    const auto& moved = scene.registry().get<CollectionMembershipComponent>(inHidden);
    CHECK_MESSAGE(moved.collections == std::vector<SceneCollectionId>{hiddenParent},
                  "members of a deleted collection should move to its parent");
}

// Scene-wide lighting follows the same visibility as meshes: the frame's sun,
// sky, and fog come from drawn primary components only.
TEST_CASE("Collection: lighting visibility") {
    Scene scene;
    SceneCollections collections;
    const SceneCollectionId hiddenCollection = create(collections, "Hidden lights");
    collections.setFlag(hiddenCollection, CollectionFlag::Visible, false);
    scene.setCollections(collections);

    CHECK_MESSAGE((frameDirectionalLight(scene).intensity == DirectionalLightComponent{}.intensity &&
                   frameEnvironmentLight(scene).diffuseIntensity == EnvironmentLightComponent{}.diffuseIntensity &&
                   !frameVolumetricFog(scene)),
                  "a scene without lights uses the default sun and sky and no fog");

    const Entity sun = scene.createEntity("Sun");
    scene.registry().emplace<DirectionalLightComponent>(sun, DirectionalLightComponent{.intensity = 3.0f});
    const Entity sky = scene.createEntity("Sky");
    scene.registry().emplace<EnvironmentLightComponent>(sky, EnvironmentLightComponent{.diffuseIntensity = 0.25f});
    const Entity fog = scene.createEntity("Fog");
    scene.registry().emplace<VolumetricFogComponent>(fog, VolumetricFogComponent{.density = 0.5f});
    CHECK_MESSAGE((primaryDirectionalLight(scene) == sun && frameDirectionalLight(scene).intensity == 3.0f &&
                   frameEnvironmentLight(scene).diffuseIntensity == 0.25f && frameVolumetricFog(scene) &&
                   frameVolumetricFog(scene)->density == 0.5f),
                  "drawn primary components light the frame");

    // The editor hide removes them from editor views only.
    scene.registry().emplace<VisibilityComponent>(sun, VisibilityComponent{.hidden = true});
    linkToCollection(scene, sky, hiddenCollection);
    scene.registry().emplace<VisibilityComponent>(fog, VisibilityComponent{.hidden = true});
    const DirectionalLightComponent hiddenSun = frameDirectionalLight(scene, SceneVisibilityMode::Editor);
    CHECK_MESSAGE((primaryDirectionalLight(scene, SceneVisibilityMode::Editor) == entt::null &&
                   hiddenSun.intensity == 0.0f && !hiddenSun.shadows.enabled),
                  "a hidden sun neither lights nor casts shadows in editor views");
    CHECK_MESSAGE((frameEnvironmentLight(scene, SceneVisibilityMode::Editor).diffuseIntensity == 0.0f &&
                   frameEnvironmentLight(scene, SceneVisibilityMode::Editor).skyColor == glm::vec3(0.0f)),
                  "an environment in a hidden collection is off in editor views");
    CHECK_MESSAGE(!frameVolumetricFog(scene, SceneVisibilityMode::Editor), "hidden fog is not drawn in editor views");
    CHECK_MESSAGE((frameDirectionalLight(scene).intensity == 3.0f &&
                   frameEnvironmentLight(scene).diffuseIntensity == 0.25f && frameVolumetricFog(scene)),
                  "the editor hide does not change runtime lighting");

    // Not renderable removes them everywhere.
    scene.registry().replace<VisibilityComponent>(sun, VisibilityComponent{.renderable = false});
    scene.registry().replace<VisibilityComponent>(fog, VisibilityComponent{.renderable = false});
    CHECK_MESSAGE((frameDirectionalLight(scene).intensity == 0.0f && !frameVolumetricFog(scene)),
                  "lights and fog that are not renderable are off at runtime too");

    // A second, drawn primary light takes over.
    const Entity backup = scene.createEntity("Backup sun");
    scene.registry().emplace<DirectionalLightComponent>(backup, DirectionalLightComponent{.intensity = 2.0f});
    CHECK_MESSAGE((primaryDirectionalLight(scene) == backup && frameDirectionalLight(scene).intensity == 2.0f),
                  "the next drawn primary light is used");
}

// The filter and expansion MeshPass::buildRenderItems applies, checked on the
// CPU: which entities draw and where collection instances place their members.
TEST_CASE("Collection: instance expansion") {
    Scene scene;
    SceneCollections collections;
    const SceneCollectionId source = create(collections, "Source");
    const SceneCollectionId nested = create(collections, "Nested", source);
    const SceneCollectionId outer = create(collections, "Outer");
    collections.setInstanceOffset(source, {1.0f, 0.0f, 0.0f});
    // The usual workflow: hide the source in the viewport, show its instances.
    collections.setFlag(source, CollectionFlag::Visible, false);
    scene.setCollections(collections);

    const Entity first = entityAt(scene, "First", {1.0f, 0.0f, 0.0f});
    const Entity second = entityAt(scene, "Second", {1.0f, 2.0f, 0.0f});
    const Entity unrendered = entityAt(scene, "Unrendered", {0.0f, 0.0f, 0.0f});
    linkToCollection(scene, first, source);
    linkToCollection(scene, second, nested);
    linkToCollection(scene, second, source);
    linkToCollection(scene, unrendered, source);
    scene.registry().emplace<VisibilityComponent>(unrendered, VisibilityComponent{.renderable = false});
    const Entity instancer = entityAt(scene, "Instancer", {0.0f, 0.0f, 10.0f});
    scene.registry().emplace<CollectionInstanceComponent>(instancer, source);
    // A second level: Outer holds an instancer of Source and is instanced itself.
    const Entity innerInstancer = entityAt(scene, "Inner instancer", {5.0f, 0.0f, 0.0f});
    scene.registry().emplace<CollectionInstanceComponent>(innerInstancer, source);
    linkToCollection(scene, innerInstancer, outer);
    const Entity outerInstancer = entityAt(scene, "Outer instancer", {0.0f, 100.0f, 0.0f});
    scene.registry().emplace<CollectionInstanceComponent>(outerInstancer, outer);
    // A cycle: Outer contains an entity instancing Outer.
    const Entity cyclic = entityAt(scene, "Cyclic", {0.0f, 0.0f, 0.0f});
    scene.registry().emplace<CollectionInstanceComponent>(cyclic, outer);
    linkToCollection(scene, cyclic, outer);
    scene.updateWorldTransforms();

    const SceneVisibility editor(scene, SceneVisibilityMode::Editor);
    CHECK_MESSAGE((!editor.drawn(first) && !editor.drawn(second)),
                  "the hidden source collection should not draw itself");
    std::vector<CollectionInstanceDraw> draws;
    expandCollectionInstances(scene, editor, draws);
    const auto drawsOf = [&draws](Entity member, Entity instancerEntity) {
        std::vector<glm::vec3> positions;
        for (const CollectionInstanceDraw& draw : draws) {
            if (draw.member == member && draw.instancer == instancerEntity) {
                positions.emplace_back(draw.world[3]);
            }
        }
        return positions;
    };
    CHECK_MESSAGE((drawsOf(first, instancer) == std::vector<glm::vec3>{{0.0f, 0.0f, 10.0f}}),
                  "an instance should place members at its origin minus the instance offset");
    CHECK_MESSAGE((drawsOf(second, instancer) == std::vector<glm::vec3>{{0.0f, 2.0f, 10.0f}}),
                  "a member of the collection and a nested one should draw once");
    CHECK_MESSAGE(drawsOf(unrendered, instancer).empty(), "non-renderable members should not be instanced");
    CHECK_MESSAGE((drawsOf(first, outerInstancer) == std::vector<glm::vec3>{{5.0f, 100.0f, 0.0f}}),
                  "nested instances should compose transforms and keep the outermost instancer");
    CHECK_MESSAGE((drawsOf(first, innerInstancer) == std::vector<glm::vec3>{{5.0f, 0.0f, 0.0f}}),
                  "the inner instancer should also draw at its own transform");
    CHECK_MESSAGE(draws.size() < 64, "instance cycles should stop expanding");

    scene.registry().emplace<VisibilityComponent>(instancer, VisibilityComponent{.hidden = true});
    std::vector<CollectionInstanceDraw> afterHide;
    expandCollectionInstances(scene, SceneVisibility(scene, SceneVisibilityMode::Editor), afterHide);
    std::vector<CollectionInstanceDraw> inGame;
    expandCollectionInstances(scene, SceneVisibility(scene, SceneVisibilityMode::Runtime), inGame);
    CHECK_MESSAGE((afterHide.size() < draws.size() && inGame.size() == draws.size()),
                  "hiding an instancer should remove its draws in the editor but not in the game");
}

TEST_CASE("Collection: document") {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    // Version 2 documents load with no collections and save as version 3.
    const std::string version2 =
        R"({"version":2,"entities":[{"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{}}]})";
    Scene migrated;
    const SceneDocumentResult loaded = SceneDocument::deserialize(version2, assets, options, migrated);
    CHECK_MESSAGE(loaded.success, (loaded.error));
    const nlohmann::json upgraded = nlohmann::json::parse(serialize(migrated, assets, options));
    CHECK_MESSAGE((upgraded.at("version") == 3 && upgraded.at("collections") == nlohmann::json::array()),
                  "a version 2 document should migrate to version 3 without collections");

    Scene scene;
    SceneCollections collections;
    const SceneCollectionId props = create(collections, "Props");
    const SceneCollectionId lights = create(collections, "Lights");
    // Moved under a newer collection, so the parent follows the child in the document.
    collections.setParent(props, lights);
    collections.setFlag(props, CollectionFlag::Selectable, false);
    collections.setFlag(lights, CollectionFlag::Visible, false);
    collections.setInstanceOffset(props, {0.5f, 1.0f, -2.0f});
    scene.setCollections(collections);
    const Entity crate = scene.createEntity("Crate");
    linkToCollection(scene, crate, props);
    linkToCollection(scene, crate, lights);
    scene.registry().emplace<VisibilityComponent>(crate, VisibilityComponent{.hidden = true, .selectable = false});
    const Entity instance = scene.createEntity("Instance");
    scene.registry().emplace<CollectionInstanceComponent>(instance, props);

    const std::string saved = serialize(scene, assets, options);
    Scene restored;
    const SceneDocumentResult restoredResult = SceneDocument::deserialize(saved, assets, options, restored);
    CHECK_MESSAGE(restoredResult.success, (restoredResult.error));
    CHECK_MESSAGE(restored.collections() == scene.collections(), "collections should round trip");
    CHECK_MESSAGE(serialize(restored, assets, options) == saved, "a version 3 document should round trip exactly");
    const Entity restoredCrate = restored.findEntity(scene.id(crate));
    CHECK_MESSAGE((restored.registry().get<CollectionMembershipComponent>(restoredCrate).collections ==
                       std::vector<SceneCollectionId>{props, lights} &&
                   restored.registry().get<VisibilityComponent>(restoredCrate) ==
                       VisibilityComponent{.hidden = true, .selectable = false}),
                  "membership and visibility should round trip");
    CHECK_MESSAGE(
        restored.registry().get<CollectionInstanceComponent>(restored.findEntity(scene.id(instance))).collection ==
            props,
        "collection instances should round trip");

    const auto rejected = [&](std::string document, std::string_view expectedError) {
        Scene target;
        const SceneDocumentResult result = SceneDocument::deserialize(document, assets, options, target);
        CHECK_MESSAGE((!result.success && result.error.find(expectedError) != std::string::npos),
                      (std::string("document should be rejected with: ") + std::string(expectedError) + " (got " +
                       result.error + ")"));
    };
    const std::string entity =
        R"({"id":1,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":)";
    const std::string collection =
        R"({"id":1,"name":"C","parent":null,"visible":true,"selectable":true,"renderable":true,"instance_offset":[0,0,0]})";
    rejected(R"({"version":3,"entities":[]})", "collections array");
    rejected(R"({"version":3,"collections":[],"entities":[)" + entity +
                 R"({"collection_membership":{"collections":[4]}}}]})",
             "missing collection 4");
    rejected(R"({"version":3,"collections":[],"entities":[)" + entity +
                 R"({"collection_instance":{"collection":4}}}]})",
             "instances missing collection 4");
    rejected(R"({"version":3,"collections":[)" + collection + "," + collection + R"(],"entities":[]})", "Duplicate");
    rejected(
        R"({"version":3,"collections":[{"id":1,"name":"C","parent":1,"visible":true,"selectable":true,"renderable":true,"instance_offset":[0,0,0]}],"entities":[]})",
        "inside itself");
    rejected(
        R"({"version":3,"collections":[{"id":1,"name":"C","parent":7,"visible":true,"selectable":true,"renderable":true,"instance_offset":[0,0,0]}],"entities":[]})",
        "parent");
    rejected(R"({"version":2,"collections":[],"entities":[]})", "version 2 document cannot define collections");
}

// Undo and redo patch collection definitions together with entities.
TEST_CASE("Collection: collection changes") {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    Scene scene;
    const Entity crate = scene.createEntity("Crate");
    const std::string before = serialize(scene, assets, options);
    SceneCollections collections;
    const SceneCollectionId props = create(collections, "Props");
    scene.setCollections(collections);
    linkToCollection(scene, crate, props);
    const std::string after = serialize(scene, assets, options);

    SceneChanges changes;
    CHECK_MESSAGE(SceneDocument::diff(before, after, changes).success, "the diff should succeed");
    CHECK_MESSAGE((changes.collections && changes.entities.size() == 1),
                  "a diff should record collections with entities");
    const uint64_t revision = scene.revisions().renderable;
    CHECK_MESSAGE(
        (SceneDocument::applyChanges(changes, SceneChangeDirection::Backward, assets, options, scene).success &&
         serialize(scene, assets, options) == before && scene.collections().empty()),
        "patching backward should remove the collection and the membership");
    CHECK_MESSAGE(scene.revisions().renderable != revision, "patching collections should invalidate renders");
    CHECK_MESSAGE(
        (SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene).success &&
         serialize(scene, assets, options) == after),
        "patching forward should restore them");
    CHECK_MESSAGE(scene.findEntity({1}) == crate, "patching should keep entity handles");

    // Dropping only the collection would strand the unchanged member.
    SceneChanges collectionsOnly{.collections = changes.collections};
    const SceneDocumentResult stranded =
        SceneDocument::applyChanges(collectionsOnly, SceneChangeDirection::Backward, assets, options, scene);
    CHECK_MESSAGE((!stranded.success && stranded.error.find("missing collection") != std::string::npos &&
                   serialize(scene, assets, options) == after),
                  "a patch leaving a dangling membership should be rejected without changes");
}

TEST_SUITE_END();
