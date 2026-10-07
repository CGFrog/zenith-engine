// CPU coverage of fast prefab spawning: the prefab document cache and its
// invalidation, instances built straight from cached documents (compared with
// a rebuild through SceneDocument::applyChanges), failed spawns leaving the
// scene's entities unchanged, overrides through the entity command buffer, and
// Scene::findChild / findDescendant.

#include "engine/assets/Assets.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("PrefabSpawn");

namespace {

// A game module's component holding an entity reference.
struct FollowComponent {
    SceneEntityId target;
};

std::shared_ptr<const ComponentTypeRegistry> testComponentTypes() {
    auto registry = std::make_shared<ComponentTypeRegistry>();
    CHECK_MESSAGE(registerBuiltInComponentTypes(*registry).success, "built-in component types register");
    CHECK_MESSAGE(registry
                      ->add(ComponentTypeBuilder<FollowComponent>("test_follow", "Follow")
                                .fields(Fields<FollowComponent>().add("target", &FollowComponent::target))
                                .build())
                      .success,
                  "a component with an entity reference registers");
    return registry;
}

struct PrefabFixture {
    explicit PrefabFixture(const std::string& name)
        : directory("prefab-spawn-" + name)
        , root(directory.path())
        , options{.assetRoot = root, .componentTypes = testComponentTypes()} {}

    // Captures root's subtree as the prefab at reference.
    void save(const Scene& scene, Entity entity, const std::filesystem::path& reference) {
        PrefabSystem prefabs(assets, options);
        PrefabCapture capture;
        CHECK_MESSAGE(prefabs.capture(scene, entity, reference, capture).success, "a test prefab is captured");
        CHECK_MESSAGE(savePrefab(root / reference, capture.document).success, "a test prefab is saved");
    }

    std::string serialize(const Scene& scene) const {
        std::string output;
        CHECK_MESSAGE(SceneDocument::serialize(scene, assets, options, output).success, "the scene serializes");
        return output;
    }

    const testsupport::TempDirectory directory;
    const std::filesystem::path root;
    AssetManager assets;
    SceneDocumentOptions options;
};

// A lamp: a point light with a child bulb.
void writeLamp(PrefabFixture& fixture, const std::string& name = "Lamp") {
    Scene scene;
    const Entity lamp = scene.createEntity(name);
    scene.registry().emplace<PointLightComponent>(lamp, glm::vec3(1.0f), 10.0f, 4.0f);
    scene.setParent(scene.createEntity("Bulb"), lamp);
    fixture.save(scene, lamp, "lamp.prefab");
}

// A room with a door that follows the room, carries a component no registry
// knows, and a nested lamp.
void writeRoom(PrefabFixture& fixture) {
    writeLamp(fixture);
    Scene scene;
    const Entity room = scene.createEntity("Room");
    scene.registry().emplace<PointLightComponent>(room, glm::vec3(0.5f), 5.0f, 8.0f);
    const Entity door = scene.createEntity("Door");
    scene.registry().emplace<FollowComponent>(door, scene.id(room));
    scene.registry().emplace<PreservedComponentsComponent>(
        door, PreservedComponentsComponent{{{"future_component", R"({"strength":3})"}}});
    scene.setLocalTransform(door, glm::mat4(glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
                                            glm::vec4(0.0f, 0.0f, 1.0f, 0.0f), glm::vec4(0.0f, 0.0f, -5.0f, 1.0f)));
    scene.setParent(door, room);
    PrefabSystem prefabs(fixture.assets, fixture.options);
    const PrefabInstanceResult lamp = prefabs.instantiate("lamp.prefab", scene, room);
    CHECK_MESSAGE(lamp.success, "the nested lamp is instantiated while authoring");
    fixture.save(scene, room, "room.prefab");
}

PrefabEntityId localNamed(const PrefabDocument& document, const std::string& name) {
    for (const PrefabEntity& entity : document.entities) {
        if (entity.name == name) {
            return entity.id;
        }
    }
    return {};
}

TEST_CASE("PrefabSpawn: document cache") {
    PrefabFixture fixture("cache");
    writeLamp(fixture);
    PrefabSystem prefabs(fixture.assets, fixture.options);
    // Every use compares the file, so changes are seen at once.
    prefabs.setFileCheckInterval(std::chrono::milliseconds(0));
    Scene scene;
    CHECK_MESSAGE(prefabs.instantiate("lamp.prefab", scene).success, "the first spawn succeeds");
    const PrefabCacheStats first = prefabs.cacheStats();
    CHECK_MESSAGE((first.loads == 1 && first.entries == 1), "the first spawn reads the prefab once");
    CHECK_MESSAGE(prefabs.instantiate("lamp.prefab", scene).success, "the second spawn succeeds");
    const PrefabCacheStats second = prefabs.cacheStats();
    CHECK_MESSAGE((second.loads == 1 && second.hits > first.hits), "the second spawn reuses the cached document");

    // A changed file is read again; its write time is moved on explicitly so
    // the check does not depend on the file system's timestamp resolution.
    const std::filesystem::path file = fixture.root / "lamp.prefab";
    const std::filesystem::file_time_type before = std::filesystem::last_write_time(file);
    writeLamp(fixture, "Lantern");
    std::filesystem::last_write_time(file, before + std::chrono::seconds(5));
    const PrefabInstanceResult changed = prefabs.instantiate("lamp.prefab", scene);
    CHECK_MESSAGE(prefabs.cacheStats().loads == 2, "a changed prefab file is read again");
    CHECK_MESSAGE((changed.success && scene.registry().get<NameComponent>(changed.root).name == "Lantern"),
                  "a spawn after the file changed builds the new content");

    // Unchanged files can be invalidated by reference or by absolute path.
    prefabs.invalidate("lamp.prefab");
    CHECK_MESSAGE(prefabs.cacheStats().entries == 0, "invalidating a reference forgets its document");
    CHECK_MESSAGE((prefabs.instantiate("lamp.prefab", scene).success && prefabs.cacheStats().loads == 3),
                  "a spawn after invalidate reads the file again");
    prefabs.invalidate(file);
    CHECK_MESSAGE((prefabs.instantiate("lamp.prefab", scene).success && prefabs.cacheStats().loads == 4),
                  "invalidating the absolute path forgets the document too");
    prefabs.clearCache();
    CHECK_MESSAGE(prefabs.cacheStats().entries == 0, "clearCache forgets every document");

    // Within the check interval the cached document is trusted until it is
    // invalidated.
    prefabs.setFileCheckInterval(std::chrono::hours(1));
    CHECK_MESSAGE(prefabs.instantiate("lamp.prefab", scene).success, "a spawn caches the lamp again");
    const std::filesystem::file_time_type lantern = std::filesystem::last_write_time(file);
    writeLamp(fixture, "Lamp");
    std::filesystem::last_write_time(file, lantern + std::chrono::seconds(5));
    const PrefabInstanceResult trusted = prefabs.instantiate("lamp.prefab", scene);
    CHECK_MESSAGE((trusted.success && scene.registry().get<NameComponent>(trusted.root).name == "Lantern"),
                  "a recently checked document is not compared with its file");
    prefabs.invalidate("lamp.prefab");
    const PrefabInstanceResult invalidated = prefabs.instantiate("lamp.prefab", scene);
    CHECK_MESSAGE((invalidated.success && scene.registry().get<NameComponent>(invalidated.root).name == "Lamp"),
                  "invalidate makes the next spawn see the written file");

    // A missing file is not cached and fails every time.
    const size_t cachedBefore = prefabs.cacheStats().entries;
    CHECK_MESSAGE((!prefabs.instantiate("missing.prefab", scene) && !prefabs.instantiate("missing.prefab", scene) &&
                   prefabs.cacheStats().entries == cachedBefore),
                  "a missing prefab fails and is not cached");
}

TEST_CASE("PrefabSpawn: instantiate matches rebuild") {
    PrefabFixture fixture("equivalence");
    writeRoom(fixture);
    PrefabSystem prefabs(fixture.assets, fixture.options);
    PrefabDocument document;
    CHECK_MESSAGE(prefabs.load("room.prefab", document).success, "the room prefab loads");
    const std::vector<PrefabOverride> overrides{
        {.entity = localNamed(document, "Room"),
         .component = "point_light",
         .field = "/intensity",
         .jsonValue = "42.0"},
        {.entity = localNamed(document, "Door"), .field = "/name", .jsonValue = R"("Front door")"},
    };

    Scene scene;
    const Entity existing = scene.createEntity("Existing");
    const PrefabInstanceResult instance = prefabs.instantiate("room.prefab", scene, existing, overrides);
    CHECK_MESSAGE(instance.success, "the room instantiates");
    if (!instance) {
        std::cerr << instance.error << '\n';
        return;
    }
    const Entity room = instance.root;
    const Entity door = scene.findChild(room, "Front door");
    const Entity lamp = scene.findChild(room, "Lamp");
    CHECK_MESSAGE((scene.registry().get<RelationshipComponent>(room).parent == existing),
                  "the instance goes under its parent");
    CHECK_MESSAGE((door != entt::null && lamp != entt::null), "overrides rename and nested prefabs instantiate");
    CHECK_MESSAGE((scene.registry().get<PointLightComponent>(room).intensity == 42.0f), "a component override applies");
    CHECK_MESSAGE((door != entt::null && scene.registry().get<FollowComponent>(door).target == scene.id(room)),
                  "entity references are remapped to the instance's ids");
    CHECK_MESSAGE((door != entt::null && scene.registry().all_of<PreservedComponentsComponent>(door)),
                  "unknown components are preserved");
    CHECK_MESSAGE((lamp != entt::null && scene.registry().get<PrefabInstanceComponent>(lamp).nestedId != 0 &&
                   scene.findDescendant(room, "Bulb") != entt::null),
                  "the nested instance remembers its record");
    CHECK_MESSAGE((scene.registry().get<PrefabInstanceComponent>(room).overrides == overrides),
                  "the overrides are persisted on the instance");
    CHECK_MESSAGE(instance.entities.size() == 4, "the result lists every entity of the instance");

    // A rebuild through SceneDocument::applyChanges (the refresh path) keeps
    // stable ids, so a matching document shows the direct build creates
    // exactly what the patch would.
    const std::string built = fixture.serialize(scene);
    CHECK_MESSAGE(prefabs.refresh(scene, room, overrides, PrefabRefreshMode::RemoveAddedObjects).success,
                  "the instance refreshes");
    CHECK_MESSAGE(fixture.serialize(scene) == built, "a direct build matches the applyChanges rebuild");

    // Round trip through a document load.
    Scene reloaded;
    CHECK_MESSAGE((SceneDocument::deserialize(built, fixture.assets, fixture.options, reloaded).success &&
                   fixture.serialize(reloaded) == built),
                  "the built instance survives a document round trip");
}

TEST_CASE("PrefabSpawn: failed instantiate leaves scene unchanged") {
    PrefabFixture fixture("failure");
    writeRoom(fixture);
    PrefabSystem prefabs(fixture.assets, fixture.options);
    Scene scene;
    const Entity existing = scene.createEntity("Existing");
    CHECK_MESSAGE(prefabs.instantiate("room.prefab", scene, existing).success, "a first room spawns");
    const std::string before = fixture.serialize(scene);

    PrefabDocument document;
    CHECK_MESSAGE(prefabs.load("room.prefab", document).success, "the room prefab loads");
    // The cached graph is valid, so this fails while building, after the
    // room's entities were created.
    const std::vector<PrefabOverride> invalid{{.entity = localNamed(document, "Room"),
                                               .component = "point_light",
                                               .field = "/intensity",
                                               .jsonValue = R"("bright")"}};
    const PrefabInstanceResult overridden = prefabs.instantiate("room.prefab", scene, existing, invalid);
    CHECK_MESSAGE((!overridden && !overridden.error.empty()), "an invalid override fails the spawn");
    CHECK_MESSAGE(fixture.serialize(scene) == before, "a spawn failing while building leaves the scene unchanged");

    CHECK_MESSAGE(!prefabs.instantiate("missing.prefab", scene, existing), "a missing prefab fails");
    CHECK_MESSAGE(fixture.serialize(scene) == before, "a missing prefab leaves the scene unchanged");

    // A broken nested prefab fails before anything is created.
    {
        std::ofstream broken(fixture.root / "lamp.prefab", std::ios::binary | std::ios::trunc);
        broken << "{ not json";
    }
    prefabs.invalidate("lamp.prefab");
    const PrefabInstanceResult nested = prefabs.instantiate("room.prefab", scene, existing);
    CHECK_MESSAGE((!nested && nested.error.find("lamp.prefab") != std::string::npos),
                  "a broken nested prefab fails the spawn and names the file");
    CHECK_MESSAGE(fixture.serialize(scene) == before, "a broken nested prefab leaves the scene unchanged");
}

TEST_CASE("PrefabSpawn: command buffer overrides") {
    PrefabFixture fixture("commands");
    writeLamp(fixture);
    PrefabDocument document;
    CHECK_MESSAGE(PrefabSystem(fixture.assets, fixture.options).load("lamp.prefab", document).success,
                  "the lamp prefab loads");
    Logger logger;
    DebugDraw debugDraw;
    GameServices services(fixture.assets, fixture.options, debugDraw, &logger);
    Scene scene;
    const std::vector<PrefabOverride> overrides{{.entity = localNamed(document, "Lamp"),
                                                 .component = "point_light",
                                                 .field = "/intensity",
                                                 .jsonValue = "3.5"}};
    Entity spawned = entt::null;
    services.world().commands().spawn("lamp.prefab", {.localTransform = glm::mat4(2.0f)}, overrides,
                                      [&spawned](Scene&, Entity entity) { spawned = entity; });
    services.world().commands().spawn("lamp.prefab");
    services.flush(scene);
    CHECK_MESSAGE((spawned != entt::null && scene.registry().get<PointLightComponent>(spawned).intensity == 3.5f),
                  "a command buffer spawn applies its overrides");
    CHECK_MESSAGE(
        (spawned != entt::null && scene.registry().get<PrefabInstanceComponent>(spawned).overrides == overrides),
        "a command buffer spawn persists its overrides");
    CHECK_MESSAGE((spawned != entt::null && scene.localTransform(spawned) == glm::mat4(2.0f)),
                  "the placement's transform still applies");
    size_t lamps = 0;
    for (const auto [entity, light] : scene.registry().view<const PointLightComponent>().each()) {
        lamps += light.intensity == 10.0f ? 1 : 0;
    }
    CHECK_MESSAGE(lamps == 1, "a spawn without overrides keeps the prefab's values");
}

TEST_CASE("PrefabSpawn: find child and descendant") {
    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity first = scene.createEntity("First");
    const Entity target = scene.createEntity("Target");
    const Entity second = scene.createEntity("Target");
    const Entity deep = scene.createEntity("Deep");
    scene.setParent(first, root);
    scene.setParent(target, first);
    scene.setParent(second, root);
    scene.setParent(deep, target);

    CHECK_MESSAGE(scene.findChild(root, "First") == first, "findChild finds a direct child");
    CHECK_MESSAGE(scene.findChild(root, "Target") == second, "findChild only looks at direct children");
    CHECK_MESSAGE((scene.findChild(root, "Deep") == entt::null), "findChild does not search grandchildren");
    CHECK_MESSAGE(scene.findDescendant(root, "Target") == target,
                  "findDescendant searches depth first: a child's subtree before its next sibling");
    CHECK_MESSAGE(scene.findDescendant(root, "Deep") == deep, "findDescendant reaches any depth");
    CHECK_MESSAGE((scene.findDescendant(root, "Root") == entt::null), "findDescendant excludes the root itself");
    CHECK_MESSAGE(
        (scene.findChild(entt::null, "First") == entt::null && scene.findDescendant(entt::null, "First") == entt::null),
        "an invalid parent finds nothing");
    CHECK_MESSAGE((scene.findDescendant(root, "Missing") == entt::null), "a missing name finds nothing");
}
} // namespace

TEST_SUITE_END();
