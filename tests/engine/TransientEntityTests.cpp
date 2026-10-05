// CPU tests of transient (generated) entities: the TransientComponent tag and
// Scene's helpers, and SceneDocument leaving them out of documents, writing
// saved references to them as none, and patching around them.

#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
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

// A gameplay component referring to another entity.
struct Follow {
    SceneEntityId target;
};

std::shared_ptr<const ComponentTypeRegistry> followRegistry() {
    auto registry = std::make_shared<ComponentTypeRegistry>();
    expect(registerBuiltInComponentTypes(*registry).success, "the built-in types register");
    expect(registry
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

void testSceneHelpers() {
    Scene scene;
    const Entity saved = scene.createEntity("Saved");
    const Entity preview = scene.createEntity("Preview");
    const Entity room = scene.createEntity("Room");
    const Entity door = scene.createEntity("Door");
    scene.setParent(room, preview);
    scene.setParent(door, room);
    expect(!scene.isTransient(room) && scene.transientEntities().empty(), "nothing is transient until tagged");

    scene.markTransient(preview);
    scene.markTransient(door);
    scene.markTransient(entt::null);
    expect(scene.isTransient(preview) && scene.isTransient(room) && scene.isTransient(door) &&
               !scene.isTransient(saved) && !scene.isTransient(entt::null),
           "a tag makes the entity and its subtree transient");
    const std::vector<Entity> transient = scene.transientEntities();
    expect(transient == std::vector<Entity>{preview, room, door},
           "transient entities are listed once each, parents first, nested tags included");

    Entity copy = entt::null;
    expect(scene.duplicateSubtree(preview, entt::null, copy).success && scene.isTransient(copy) &&
               scene.registry().all_of<TransientComponent>(copy),
           "a duplicate of a tagged entity is tagged too");
    expect(scene.destroyTransient() == 6 && scene.registry().valid(saved) && !scene.registry().valid(preview) &&
               !scene.registry().valid(door) && scene.transientEntities().empty(),
           "destroyTransient destroys every transient entity and nothing else");
    expect(scene.destroyTransient() == 0, "destroying again finds nothing");
}

void testDocuments(AssetManager& assets) {
    const SceneDocumentOptions options{.assetRoot = std::filesystem::temp_directory_path(),
                                       .componentTypes = followRegistry()};
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
    expect(serialized.success && !contains(text, "Preview") && !contains(text, "\"Room\"") && contains(text, "Level") &&
               contains(text, "Follower"),
           "serialize leaves transient entities out");
    expect(serialized.warnings.size() == 1 && contains(serialized.warnings[0], "follow/target") &&
               contains(serialized.warnings[0], std::to_string(scene.id(room).value)),
           "a saved reference to a transient entity is reported");

    Scene loaded;
    expect(SceneDocument::deserialize(text, assets, options, loaded).success, "the document loads");
    const Entity loadedFollower = loaded.findEntity(scene.id(follower));
    const Entity loadedLoyal = loaded.findEntity(scene.id(loyal));
    expect(loadedFollower != entt::null && !loaded.registry().get<Follow>(loadedFollower).target &&
               loadedLoyal != entt::null && loaded.registry().get<Follow>(loadedLoyal).target == scene.id(level),
           "the reference to the transient entity is saved as none; others are kept");
    expect(scene.registry().get<Follow>(follower).target == scene.id(room), "saving leaves the scene's reference");

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "zenith_transient_entities.zscene";
    const SceneDocumentResult saved = SceneDocument::save(path, scene, assets, options);
    expect(saved.success && saved.warnings.size() == 1, "save reports the serialize warnings");
    std::error_code error;
    std::filesystem::remove(path, error);

    std::vector<std::optional<std::string>> entities;
    const std::vector<SceneEntityId> ids{scene.id(room), scene.id(follower)};
    expect(SceneDocument::serializeEntities(scene, assets, options, ids, entities).success && entities.size() == 2 &&
               !entities[0] && entities[1] && contains(*entities[1], "\"target\":null"),
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
    expect(SceneDocument::diff(before, after, changes).success && changes.entities.size() == 1,
           "the removal is one change");
    const SceneDocumentResult patched =
        SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene);
    expect(patched.success && scene.findEntity(levelId) == entt::null && !scene.registry().valid(room) &&
               scene.transientEntities().empty(),
           "a patch removing a saved entity removes its transient children");
}
} // namespace

int main() {
    AssetManager assets;
    testSceneHelpers();
    testDocuments(assets);
    if (failures == 0) {
        std::cout << "Transient entity tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
