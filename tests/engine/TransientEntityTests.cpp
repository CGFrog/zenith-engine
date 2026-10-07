// CPU tests of transient (generated) entities: the TransientComponent tag and
// Scene's helpers, and SceneDocument leaving them out of documents, writing
// saved references to them as none, and patching around them.

#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
// A gameplay component referring to another entity.
struct Follow {
    SceneEntityId target;
};

std::shared_ptr<const ComponentTypeRegistry> followRegistry() {
    auto registry = std::make_shared<ComponentTypeRegistry>();
    CHECK_MESSAGE(registerBuiltInComponentTypes(*registry).success, "the built-in types register");
    CHECK_MESSAGE(registry
                      ->add(ComponentTypeBuilder<Follow>("follow", "Follow")
                                .fields(Fields<Follow>().add("target", &Follow::target).optional())
                                .build())
                      .success,
                  "the follow type registers");
    return registry;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // namespace

TEST_SUITE_BEGIN("TransientEntity");

TEST_CASE("TransientEntity: scene helpers") {
    Scene scene;
    const Entity saved = scene.createEntity("Saved");
    const Entity preview = scene.createEntity("Preview");
    const Entity room = scene.createEntity("Room");
    const Entity door = scene.createEntity("Door");
    scene.setParent(room, preview);
    scene.setParent(door, room);
    CHECK_MESSAGE((!scene.isTransient(room) && scene.transientEntities().empty()), "nothing is transient until tagged");

    scene.markTransient(preview);
    scene.markTransient(door);
    scene.markTransient(entt::null);
    CHECK_MESSAGE((scene.isTransient(preview) && scene.isTransient(room) && scene.isTransient(door) &&
                   !scene.isTransient(saved) && !scene.isTransient(entt::null)),
                  "a tag makes the entity and its subtree transient");
    const std::vector<Entity> transient = scene.transientEntities();
    CHECK_MESSAGE((transient == std::vector<Entity>{preview, room, door}),
                  "transient entities are listed once each, parents first, nested tags included");

    Entity copy = entt::null;
    CHECK_MESSAGE((scene.duplicateSubtree(preview, entt::null, copy).success && scene.isTransient(copy) &&
                   scene.registry().all_of<TransientComponent>(copy)),
                  "a duplicate of a tagged entity is tagged too");
    CHECK_MESSAGE((scene.destroyTransient() == 6 && scene.registry().valid(saved) && !scene.registry().valid(preview) &&
                   !scene.registry().valid(door) && scene.transientEntities().empty()),
                  "destroyTransient destroys every transient entity and nothing else");
    CHECK_MESSAGE(scene.destroyTransient() == 0, "destroying again finds nothing");
}

TEST_CASE("TransientEntity: documents") {
    AssetManager assets;
    const testsupport::TempDirectory root("transient-entity-documents");
    const SceneDocumentOptions options{.assetRoot = root.path(), .componentTypes = followRegistry()};
    Scene scene;
    const Entity level = scene.createEntity("Level");
    const Entity preview = scene.createEntity("Preview");
    const Entity room = scene.createEntity("Room");
    scene.setParent(room, preview);
    scene.setParent(preview, level);
    scene.markTransient(preview);
    const Entity follower = scene.createEntity("Follower");
    scene.registry().emplace<Follow>(follower, Follow{.target = scene.id(room)});
    const Entity loyal = scene.createEntity("Loyal");
    scene.registry().emplace<Follow>(loyal, Follow{.target = scene.id(level)});

    std::string text;
    const SceneDocumentResult serialized = SceneDocument::serialize(scene, assets, options, text);
    CHECK_MESSAGE((serialized.success && !contains(text, "Preview") && !contains(text, "\"Room\"") &&
                   contains(text, "Level") && contains(text, "Follower")),
                  "serialize leaves transient entities out");
    CHECK_MESSAGE((serialized.warnings.size() == 1 && contains(serialized.warnings[0], "follow/target") &&
                   contains(serialized.warnings[0], std::to_string(scene.id(room).value))),
                  "a saved reference to a transient entity is reported");

    Scene loaded;
    CHECK_MESSAGE(SceneDocument::deserialize(text, assets, options, loaded).success, "the document loads");
    const Entity loadedFollower = loaded.findEntity(scene.id(follower));
    const Entity loadedLoyal = loaded.findEntity(scene.id(loyal));
    CHECK_MESSAGE((loadedFollower != entt::null && !loaded.registry().get<Follow>(loadedFollower).target &&
                   loadedLoyal != entt::null && loaded.registry().get<Follow>(loadedLoyal).target == scene.id(level)),
                  "the reference to the transient entity is saved as none; others are kept");
    CHECK_MESSAGE(scene.registry().get<Follow>(follower).target == scene.id(room),
                  "saving leaves the scene's reference");

    const std::filesystem::path path = root / "zenith_transient_entities.zscene";
    const SceneDocumentResult saved = SceneDocument::save(path, scene, assets, options);
    CHECK_MESSAGE((saved.success && saved.warnings.size() == 1), "save reports the serialize warnings");

    std::vector<std::optional<std::string>> entities;
    const std::vector<SceneEntityId> ids{scene.id(room), scene.id(follower)};
    CHECK_MESSAGE((SceneDocument::serializeEntities(scene, assets, options, ids, entities).success &&
                   entities.size() == 2 && !entities[0] && entities[1] && contains(*entities[1], "\"target\":null")),
                  "serializeEntities reports transient entities as absent and drops references to them");

    // Removing a saved entity takes its transient children along, as a
    // document that never listed them expects.
    const SceneEntityId levelId = scene.id(level);
    std::string before;
    SceneDocument::serialize(scene, assets, options, before);
    Scene removed;
    SceneDocument::deserialize(before, assets, options, removed);
    removed.destroyEntity(removed.findEntity(levelId));
    std::string after;
    SceneDocument::serialize(removed, assets, options, after);
    SceneChanges changes;
    CHECK_MESSAGE((SceneDocument::diff(before, after, changes).success && changes.entities.size() == 1),
                  "the removal is one change");
    const SceneDocumentResult patched =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene);
    CHECK_MESSAGE((patched.success && scene.findEntity(levelId) == entt::null && !scene.registry().valid(room) &&
                   scene.transientEntities().empty()),
                  "a patch removing a saved entity removes its transient children");
}

TEST_SUITE_END();
