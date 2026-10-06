#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/document/PrefabEditDocument.hpp"
#include "editor/document/SceneFragment.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "engine/scene/Collections.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// A game module's component, registered beside the built-in types.
struct HealthComponent {
    float health{100.0f};
    std::string team;
};

// A module component holding entity references, one nested in a struct.
struct FollowSettings {
    SceneEntityId target;
    float distance{1.0f};
};

struct FollowComponent {
    SceneEntityId target;
    FollowSettings settings;
};

std::shared_ptr<const ComponentTypeRegistry> moduleComponentTypes() {
    auto registry = std::make_shared<ComponentTypeRegistry>();
    expect(registerBuiltInComponentTypes(*registry).success, "built-in component types should register");
    expect(registry
               ->add(ComponentTypeBuilder<FollowComponent>("test_follow", "Follow")
                         .fields(Fields<FollowComponent>()
                                     .add("target", &FollowComponent::target)
                                     .structure("settings", &FollowComponent::settings,
                                                Fields<FollowSettings>()
                                                    .add("target", &FollowSettings::target)
                                                    .add("distance", &FollowSettings::distance)))
                         .build())
               .success,
           "a component with entity references should register");
    expect(registry
               ->add(ComponentTypeBuilder<HealthComponent>("test_health", "Health")
                         .fields(Fields<HealthComponent>()
                                     .add("health", &HealthComponent::health)
                                     .add("team", &HealthComponent::team))
                         .build())
               .success,
           "a module component type should register");
    return registry;
}

std::filesystem::path freshDirectory(const std::string& name) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("zenith-prefab-tests-" + name);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

Entity childNamed(const Scene& scene, Entity parent, const std::string& name) {
    for (const Entity child : scene.registry().get<RelationshipComponent>(parent).children) {
        if (scene.registry().get<NameComponent>(child).name == name) {
            return child;
        }
    }
    return entt::null;
}

// A lamp with a point light, a module component, and a component whose type
// is not registered, plus a child camera.
Entity authorLamp(Scene& scene) {
    const Entity lamp = scene.createEntity("Lamp");
    scene.registry().emplace<PointLightComponent>(lamp, glm::vec3(1.0f), 10.0f, 4.0f);
    scene.registry().emplace<HealthComponent>(lamp, 50.0f, "red");
    scene.registry().emplace<PreservedComponentsComponent>(
        lamp, PreservedComponentsComponent{{{"future_component", R"({"strength":3})"}}});
    const Entity bulb = scene.createEntity("Bulb");
    scene.registry().emplace<CameraComponent>(bulb);
    scene.registry().get<TransformComponent>(bulb).local[3] = glm::vec4(0.0f, 2.0f, 0.0f, 1.0f);
    scene.setParent(bulb, lamp);
    scene.updateWorldTransforms();
    return lamp;
}

void testRegistryRoundTrip() {
    const std::filesystem::path root = freshDirectory("registry");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root, .componentTypes = moduleComponentTypes()};
    Scene scene;
    const Entity lamp = authorLamp(scene);
    PrefabAuthoring authoring(assets, options);
    const PrefabResult created = authoring.create(scene, lamp, "props/lamp.prefab");
    expect(created.success, "a prefab should be created from any registered components");
    expect(!authoring.create(scene, lamp, "props/lamp.prefab"), "create should not overwrite an existing prefab");
    expect(!authoring.create(scene, lamp, "props/lamp.txt"), "prefab files should need the .prefab extension");
    expect(!authoring.create(scene, lamp, "../outside.prefab"), "prefabs should stay inside the asset root");

    PrefabDocument document;
    expect(loadPrefab(root / "props/lamp.prefab", document).success && document.entities.size() == 2,
           "the prefab file should load with both entities");
    const auto hasComponent = [&document](size_t index, const std::string& key) {
        return std::ranges::find(document.entities[index].components, key,
                                 &std::pair<std::string, std::string>::first) !=
               document.entities[index].components.end();
    };
    expect(hasComponent(0, "point_light") && hasComponent(0, "test_health") && hasComponent(0, "future_component"),
           "built-in, module, and preserved unknown components should be written");
    expect(!hasComponent(0, "prefab_entity") && !hasComponent(0, "prefab_instance"),
           "link components should not be part of the prefab content");
    expect(scene.registry().all_of<PrefabInstanceComponent>(lamp) &&
               scene.registry().get<PrefabInstanceComponent>(lamp).prefab == "props/lamp.prefab",
           "create should link the authored entity to the new prefab");

    // Without the module, its component survives as preserved data.
    Scene builtInScene;
    PrefabSystem builtInPrefabs(assets, SceneDocumentOptions{.assetRoot = root});
    const PrefabInstanceResult withoutModule = builtInPrefabs.instantiate("props/lamp.prefab", builtInScene);
    const auto* preserved =
        withoutModule ? builtInScene.registry().try_get<PreservedComponentsComponent>(withoutModule.root) : nullptr;
    expect(preserved != nullptr && preserved->components.size() == 2,
           "module and unknown components should be preserved when their types are not registered");

    Scene moduleScene;
    PrefabSystem modulePrefabs(assets, options);
    const PrefabInstanceResult withModule = modulePrefabs.instantiate("props/lamp.prefab", moduleScene);
    const auto* health = withModule ? moduleScene.registry().try_get<HealthComponent>(withModule.root) : nullptr;
    expect(health != nullptr && health->health == 50.0f && health->team == "red",
           "module components should load through the registry");
    expect(withModule.entities.size() == 2 &&
               moduleScene.registry().all_of<CameraComponent>(childNamed(moduleScene, withModule.root, "Bulb")),
           "child entities and their components should be instantiated");
    std::vector<PrefabOverride> overrides;
    expect(modulePrefabs.overrides(moduleScene, withModule.root, overrides).success && overrides.empty(),
           "a fresh instance should have no overrides");
}

void testVersion1Migration() {
    const std::filesystem::path root = freshDirectory("v1");
    {
        std::ofstream child(root / "child.prefab");
        child << R"({"version": 1, "entities": [
            {"id": 1, "name": "Light", "parent": null,
             "localTransform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
             "components": {"pointLight": {"color": [1, 0.5, 0.25], "intensity": 7, "range": 3, "castsShadows": true},
                            "camera": {"verticalFov": 1.0, "nearPlane": 0.5, "farPlane": 50}}},
            {"id": 2, "name": "Second root", "parent": null,
             "localTransform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]}],
          "nested": []})";
    }
    {
        std::ofstream parent(root / "parent.prefab");
        parent << R"({"version": 1, "entities": [
            {"id": 1, "name": "Holder", "parent": null,
             "localTransform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]}],
          "nested": [{"id": 5, "source": "child.prefab", "parent": 1, "overrides": [
            {"entity": 1, "property": "name", "value": "Renamed"},
            {"entity": 1, "property": "component.pointLight",
             "value": {"color": [0, 1, 0], "intensity": 99, "range": 2}},
            {"entity": 1, "property": "unknownProperty", "value": 1}]}]})";
    }
    PrefabDocument child;
    const PrefabResult loaded = loadPrefab(root / "child.prefab", child);
    expect(loaded.success && child.entities.size() == 2, "version 1 prefabs should load");
    expect(loaded.diagnostics.size() == 1, "a second version 1 root should be reported");
    expect(child.entities[1].parent == PrefabEntityId{1}, "a second version 1 root should become a child");
    const auto& components = child.entities[0].components;
    const auto pointLight = std::ranges::find(components, "point_light", &std::pair<std::string, std::string>::first);
    expect(pointLight != components.end() && nlohmann::json::parse(pointLight->second).value("casts_shadows", false),
           "version 1 point lights should migrate to registry keys");

    PrefabDocument parent;
    const PrefabResult parentLoaded = loadPrefab(root / "parent.prefab", parent);
    expect(parentLoaded.success && parent.nested.size() == 1 && parent.nested.front().overrides.size() == 2 &&
               parentLoaded.diagnostics.size() == 1,
           "version 1 overrides should migrate and unsupported ones should be reported");
    expect(parent.nested.front().overrides[0].field == "/name" &&
               parent.nested.front().overrides[1].component == "point_light",
           "version 1 override properties should become field and component overrides");

    AssetManager assets;
    PrefabSystem prefabs(assets, SceneDocumentOptions{.assetRoot = root});
    Scene scene;
    const PrefabInstanceResult instance = prefabs.instantiate("parent.prefab", scene);
    expect(instance.success && instance.entities.size() == 3, "a migrated nested prefab should instantiate");
    const Entity light = instance ? childNamed(scene, instance.root, "Renamed") : entt::null;
    expect(light != entt::null && scene.registry().get<PointLightComponent>(light).intensity == 99.0f &&
               scene.registry().get<CameraComponent>(light).nearPlane == 0.5f,
           "migrated components and overrides should apply");

    std::string encoded;
    PrefabDocument reloaded;
    expect(serializePrefab(child, encoded).success && encoded.find("\"version\": 2") != std::string::npos &&
               deserializePrefab(encoded, reloaded).success && reloaded.entities.size() == 2,
           "migrated prefabs should save as version 2");
}

void testLinkPersistence() {
    const std::filesystem::path root = freshDirectory("persistence");
    AssetManager assets;
    auto database = std::make_shared<AssetDatabase>(root);
    SceneDocumentOptions options{.assetRoot = root, .assetDatabase = database};
    Scene authored;
    PrefabAuthoring authoring(assets, options);
    expect(authoring.create(authored, authorLamp(authored), "lamp.prefab").success,
           "the persistence fixture prefab should be created");
    expect(database->rescan().success, "the asset database should scan the new prefab");

    Scene scene;
    Entity instance = entt::null;
    expect(authoring.instantiate(scene, "lamp.prefab", entt::null, instance).success, "the prefab should instantiate");
    const Entity bulb = childNamed(scene, instance, "Bulb");
    scene.registry().get<CameraComponent>(bulb).farPlane = 123.0f;
    const auto scenePath = root / "level.zenith-scene";
    expect(SceneDocument::save(scenePath, scene, assets, options).success, "the scene should save");
    const std::string saved = readText(scenePath);
    expect(saved.find("\"prefab_instance\"") != std::string::npos &&
               saved.find("\"prefab_entity\"") != std::string::npos && saved.find("\"guid\"") != std::string::npos,
           "the prefab link should be saved with the prefab's GUID");

    expect(database->move("lamp.prefab", "moved/lamp.prefab").success, "the prefab should move with its GUID");
    Scene reloaded;
    expect(SceneDocument::load(scenePath, assets, options, reloaded).success, "the scene should load");
    const Entity loadedRoot = reloaded.findEntity(scene.id(instance));
    expect(loadedRoot != entt::null &&
               reloaded.registry().get<PrefabInstanceComponent>(loadedRoot).prefab == "moved/lamp.prefab",
           "the loaded link should follow the moved prefab by GUID");
    PrefabEntityOverrides overrides;
    const Entity loadedBulb = reloaded.findEntity(scene.id(bulb));
    expect(authoring.overrides(reloaded, loadedBulb, overrides).success && overrides.linked &&
               overrides.overridden("camera", "/far_plane") && !overrides.overridden("camera", "/near_plane"),
           "overrides should be found after the link is reloaded");
    expect(authoring.revert(reloaded, loadedBulb).success &&
               reloaded.registry().get<CameraComponent>(reloaded.findEntity(scene.id(bulb))).farPlane == 1000.0f,
           "a reloaded instance should revert to its prefab");
}

void testOverrideQuery() {
    const std::filesystem::path root = freshDirectory("overrides");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root, .componentTypes = moduleComponentTypes()};
    Scene scene;
    PrefabAuthoring authoring(assets, options);
    const Entity lamp = authorLamp(scene);
    expect(authoring.create(scene, lamp, "lamp.prefab").success, "the override fixture prefab should be created");
    const Entity bulb = childNamed(scene, lamp, "Bulb");

    scene.registry().get<PointLightComponent>(lamp).intensity = 42.0f;
    scene.registry().get<TransformComponent>(lamp).local[3] = glm::vec4(5.0f, 0.0f, 0.0f, 1.0f);
    scene.registry().emplace<SpotLightComponent>(lamp);
    scene.registry().remove<CameraComponent>(bulb);
    scene.registry().get<NameComponent>(bulb).name = "Renamed bulb";
    scene.registry().get<TransformComponent>(bulb).local[3] = glm::vec4(0.0f, 3.0f, 0.0f, 1.0f);

    PrefabEntityOverrides lampOverrides;
    expect(authoring.overrides(scene, lamp, lampOverrides).success && lampOverrides.linked &&
               lampOverrides.instanceRoot == scene.id(lamp) && !lampOverrides.added,
           "the instance root should report its link");
    expect(!lampOverrides.localTransform && !lampOverrides.name,
           "moving the instance root should not count as an override");
    const auto pointLight =
        std::ranges::find(lampOverrides.components, "point_light", &PrefabComponentOverrides::component);
    expect(pointLight != lampOverrides.components.end() && pointLight->fields == std::vector<std::string>{"/intensity"},
           "a changed field should be reported by its JSON pointer");
    expect(lampOverrides.overridden("point_light", "/intensity") &&
               !lampOverrides.overridden("point_light", "/range") && lampOverrides.overridden("point_light", ""),
           "overridden should match the field, its component, but not other fields");
    const auto spot = std::ranges::find(lampOverrides.components, "spot_light", &PrefabComponentOverrides::component);
    expect(spot != lampOverrides.components.end() && spot->added && !spot->removed,
           "an added component should be reported");

    PrefabEntityOverrides bulbOverrides;
    expect(authoring.overrides(scene, bulb, bulbOverrides).success && bulbOverrides.name &&
               bulbOverrides.localTransform,
           "child name and transform changes should be reported");
    const auto camera = std::ranges::find(bulbOverrides.components, "camera", &PrefabComponentOverrides::component);
    expect(camera != bulbOverrides.components.end() && camera->removed, "a removed component should be reported");

    const Entity added = scene.createEntity("Added");
    scene.setParent(added, lamp);
    PrefabEntityOverrides addedOverrides;
    expect(authoring.overrides(scene, added, addedOverrides).success && addedOverrides.linked && addedOverrides.added,
           "entities added to an instance should be reported as added");
    PrefabEntityOverrides unlinked;
    const Entity loose = scene.createEntity("Loose");
    expect(authoring.overrides(scene, loose, unlinked).success && !unlinked.linked,
           "entities outside instances should not be linked");

    expect(authoring.revertField(scene, lamp, "point_light", "/intensity").success &&
               scene.registry().get<PointLightComponent>(lamp).intensity == 10.0f &&
               scene.registry().all_of<SpotLightComponent>(lamp),
           "revertField should reset one value and keep other overrides");
    expect(scene.findEntity(scene.id(added)) != entt::null, "revertField should keep added entities");
    expect(scene.registry().get<TransformComponent>(lamp).local[3].x == 5.0f,
           "refreshing should keep the instance root's placement");
    expect(authoring.revertField(scene, bulb, "", "/name").success &&
               scene.registry().get<NameComponent>(bulb).name == "Bulb",
           "revertField should reset an entity name");
    expect(!authoring.revertField(scene, bulb, "", "/name"), "reverting a value that is not overridden should fail");
}

// A document, history, and selection editing a scene headlessly, as the
// editor session owns them.
struct OperatorFixture {
    std::filesystem::path root;
    AssetManager assets;
    Scene scene;
    EditorDocument document;
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};

    explicit OperatorFixture(std::filesystem::path assetRoot)
        : root(std::move(assetRoot))
        , document(root) {
        // The object operators include the outliner's (outliner.parent_drop).
        expect(registerObjectOperators(registry).success && registerPrefabOperators(registry).success,
               "the object and prefab operators should register");
        document.setComponentTypes(moduleComponentTypes());
    }

    SceneEditing editing() {
        return SceneEditing(scene, assets, document, history, diagnostics);
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context{.editing = editing(), .selection = selection};
        return runner.invokeWithJson(id, context, properties);
    }

    void select(Entity entity) {
        selection.selectOnly(scene.id(entity));
    }

    void edit(const std::function<void()>& change) {
        editing().perform("Edit", {}, [&change] {
            change();
            return SceneResult{.success = true};
        });
    }

    Entity find(SceneEntityId id) const {
        return scene.findEntity(id);
    }
};

void testOperators() {
    OperatorFixture fixture(freshDirectory("operators"));
    Scene& scene = fixture.scene;
    const Entity lamp = authorLamp(scene);
    const SceneEntityId lampId = scene.id(lamp);

    expect(fixture.invoke("prefab.create_from_selection", R"({"path":"lamp.prefab"})").status ==
               OperatorStatus::Cancelled,
           "create should need an active entity");
    fixture.select(lamp);
    expect(fixture.invoke("prefab.create_from_selection", R"({"path":"lamp.prefab"})").status ==
               OperatorStatus::Finished,
           "prefab.create_from_selection should create and link a prefab");
    const std::filesystem::path prefabPath = fixture.root / "lamp.prefab";
    expect(std::filesystem::is_regular_file(prefabPath) &&
               scene.registry().all_of<PrefabInstanceComponent>(fixture.find(lampId)),
           "the prefab file and link should exist");
    const std::string created = readText(prefabPath);
    fixture.editing().undo();
    expect(!scene.registry().all_of<PrefabInstanceComponent>(fixture.find(lampId)) &&
               !std::filesystem::exists(prefabPath),
           "undoing create should unlink the entity and remove the written file");
    fixture.editing().redo();
    expect(scene.registry().all_of<PrefabInstanceComponent>(fixture.find(lampId)) && readText(prefabPath) == created,
           "redo should restore the link and the file");

    fixture.selection.clear();
    expect(fixture.invoke("prefab.instantiate", R"({"asset":"lamp.prefab"})").status == OperatorStatus::Finished,
           "prefab.instantiate should add an instance");
    const SceneEntityId firstId = *fixture.selection.active();
    expect(fixture.invoke("prefab.instantiate", R"({"asset":"lamp.prefab"})").status == OperatorStatus::Finished,
           "a prefab should instantiate more than once");
    const SceneEntityId secondId = *fixture.selection.active();
    const size_t entityCount = scene.registry().view<const SceneEntityIdComponent>().size();
    fixture.editing().undo();
    expect(fixture.find(secondId) == entt::null, "undo should remove the latest instance");
    fixture.editing().redo();
    expect(fixture.find(secondId) != entt::null &&
               scene.registry().view<const SceneEntityIdComponent>().size() == entityCount,
           "redo should restore the instance");
    expect(fixture.invoke("prefab.instantiate", R"({"asset":"missing.prefab"})").status == OperatorStatus::Failed &&
               scene.registry().view<const SceneEntityIdComponent>().size() == entityCount,
           "instantiating a missing prefab should fail without changing the scene");
    fixture.diagnostics.acknowledgeErrors();

    // The second instance keeps its own override through an apply.
    const SceneEntityId secondBulbId = scene.id(childNamed(scene, fixture.find(secondId), "Bulb"));
    fixture.edit([&] { scene.registry().get<NameComponent>(fixture.find(secondBulbId)).name = "Second bulb"; });
    fixture.edit([&] {
        scene.registry().get<PointLightComponent>(fixture.find(firstId)).intensity = 77.0f;
        scene.registry().get<HealthComponent>(fixture.find(firstId)).team = "blue";
    });
    fixture.select(fixture.find(firstId));
    const std::string beforeApply = readText(prefabPath);
    const size_t stepsBeforeApply = fixture.history.size();
    expect(fixture.invoke("prefab.apply").status == OperatorStatus::Finished,
           "prefab.apply should write the instance into its prefab");
    expect(fixture.history.size() == stepsBeforeApply + 1, "apply should record exactly one undo step");
    const std::string afterApply = readText(prefabPath);
    expect(readText(prefabPath).find("\"blue\"") != std::string::npos, "apply should update the prefab file");
    expect(scene.registry().get<PointLightComponent>(fixture.find(secondId)).intensity == 77.0f &&
               scene.registry().get<HealthComponent>(fixture.find(secondId)).team == "blue" &&
               scene.registry().get<PointLightComponent>(fixture.find(lampId)).intensity == 77.0f,
           "apply should update the other instances");
    expect(scene.registry().get<NameComponent>(fixture.find(secondBulbId)).name == "Second bulb",
           "apply should keep the other instances' own overrides");
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    PrefabEntityOverrides firstOverrides;
    expect(authoring.overrides(scene, fixture.find(firstId), firstOverrides).success &&
               firstOverrides.components.empty(),
           "the applied instance should have no overrides left");
    fixture.editing().undo();
    expect(scene.registry().get<PointLightComponent>(fixture.find(secondId)).intensity == 10.0f &&
               scene.registry().get<PointLightComponent>(fixture.find(firstId)).intensity == 77.0f,
           "undoing apply should restore the other instances and keep the applied instance's edit");
    expect(readText(prefabPath) == beforeApply, "undoing apply should restore the prefab file");
    fixture.editing().redo();
    expect(scene.registry().get<PointLightComponent>(fixture.find(secondId)).intensity == 77.0f &&
               readText(prefabPath) == afterApply,
           "redoing apply should write the file and update the instances again");

    // Revert drops overrides and added objects.
    // Recording an edit recreates the entities it created, so keep the stable id.
    SceneEntityId addedId;
    fixture.edit([&] {
        scene.registry().get<PointLightComponent>(fixture.find(secondId)).range = 99.0f;
        const Entity addedChild = scene.createEntity("Added child");
        scene.setParent(addedChild, fixture.find(secondId));
        addedId = scene.id(addedChild);
    });
    fixture.select(fixture.find(secondBulbId));
    expect(fixture.invoke("prefab.revert").status == OperatorStatus::Finished,
           "prefab.revert should reset the instance");
    expect(scene.registry().get<PointLightComponent>(fixture.find(secondId)).range == 4.0f &&
               scene.registry().get<NameComponent>(fixture.find(secondBulbId)).name == "Bulb" &&
               fixture.find(addedId) == entt::null,
           "revert should drop overrides and added entities and keep stable ids");
    fixture.editing().undo();
    expect(scene.registry().get<PointLightComponent>(fixture.find(secondId)).range == 99.0f &&
               fixture.find(addedId) != entt::null,
           "undoing revert should restore the overrides and added entities");

    // revert_field resets one value, addressed by entity id.
    const std::string revertRange =
        R"({"entity":")" + std::to_string(secondId.value) + R"(","component":"point_light","field":"/range"})";
    expect(fixture.invoke("prefab.revert_field", revertRange).status == OperatorStatus::Finished &&
               scene.registry().get<PointLightComponent>(fixture.find(secondId)).range == 4.0f &&
               fixture.find(addedId) != entt::null &&
               scene.registry().get<NameComponent>(fixture.find(secondBulbId)).name == "Second bulb",
           "prefab.revert_field should reset only the named value");
    fixture.editing().undo();
    expect(scene.registry().get<PointLightComponent>(fixture.find(secondId)).range == 99.0f,
           "undoing revert_field should restore the value");
    expect(fixture.invoke("prefab.revert_field", R"({"entity":"x1","field":"/name"})").status == OperatorStatus::Failed,
           "revert_field should reject an invalid entity id");
    fixture.diagnostics.acknowledgeErrors();

    // Unpack keeps the entities without the link.
    fixture.select(fixture.find(secondId));
    expect(fixture.invoke("prefab.unpack").status == OperatorStatus::Finished &&
               !scene.registry().all_of<PrefabInstanceComponent>(fixture.find(secondId)) &&
               !scene.registry().all_of<PrefabEntityComponent>(fixture.find(secondBulbId)) &&
               fixture.find(secondBulbId) != entt::null,
           "prefab.unpack should keep the entities and remove their link");
    expect(fixture.invoke("prefab.apply").status == OperatorStatus::Cancelled,
           "instance operators should not run on unlinked entities");
    fixture.editing().undo();
    expect(scene.registry().all_of<PrefabInstanceComponent>(fixture.find(secondId)), "undo unpack should relink");
}

// Add > Character is a prefab: the first use writes it, linking the new
// character to it; later uses instantiate it. Undo takes back the scene change
// and, for the first, the file.
void testAddCharacter() {
    OperatorFixture fixture(freshDirectory("character"));
    Scene& scene = fixture.scene;
    const std::filesystem::path prefabPath = fixture.root / "prefabs" / "Character.prefab";
    expect(fixture.invoke("prefab.add_character", R"({"location":[2,0,3]})").status == OperatorStatus::Finished,
           "prefab.add_character should add a character");
    expect(std::filesystem::is_regular_file(prefabPath), "the first character should write the Character prefab");
    const std::optional<SceneEntityId> firstId = fixture.selection.active();
    const Entity first = firstId ? fixture.find(*firstId) : entt::null;
    expect(first != entt::null && scene.registry().all_of<CharacterControllerComponent>(first) &&
               scene.registry().all_of<PrefabInstanceComponent>(first) &&
               scene.registry().get<PrefabInstanceComponent>(first).prefab == "prefabs/Character.prefab",
           "the character is an instance of the Character prefab");
    scene.updateWorldTransforms();
    const glm::vec4 firstPosition =
        first != entt::null ? scene.registry().get<TransformComponent>(first).world[3] : glm::vec4(0.0f);
    expect(std::abs(firstPosition.x - 2.0f) < 1e-4f && std::abs(firstPosition.z - 3.0f) < 1e-4f &&
               firstPosition.y > 0.5f,
           "the character stands on the location");
    const Entity eye = first != entt::null ? childNamed(scene, first, "Player Camera") : entt::null;
    expect(eye != entt::null && scene.registry().all_of<FirstPersonCameraComponent>(eye),
           "the prefab holds the first-person camera");
    const std::string written = readText(prefabPath);

    expect(fixture.invoke("prefab.add_character", R"({"location":[-2,0,0]})").status == OperatorStatus::Finished &&
               readText(prefabPath) == written,
           "a second character instantiates the existing prefab without rewriting it");
    const std::optional<SceneEntityId> secondId = fixture.selection.active();
    expect(secondId && *secondId != *firstId &&
               scene.registry().all_of<PrefabInstanceComponent>(fixture.find(*secondId)),
           "the second character is another instance");
    fixture.editing().undo();
    expect(fixture.find(*secondId) == entt::null && std::filesystem::exists(prefabPath),
           "undoing the second character keeps the prefab");
    fixture.editing().undo();
    expect(fixture.find(*firstId) == entt::null && !std::filesystem::exists(prefabPath),
           "undoing the first character removes it and the prefab file");
}

void testNestedPrefabs() {
    const std::filesystem::path root = freshDirectory("nested");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root};
    PrefabAuthoring authoring(assets, options);
    Scene scene;
    const Entity wheel = scene.createEntity("Wheel");
    scene.registry().emplace<PointLightComponent>(wheel);
    expect(authoring.create(scene, wheel, "parts/wheel.prefab").success, "the inner prefab should be created");

    const Entity cart = scene.createEntity("Cart");
    scene.setParent(wheel, cart);
    scene.registry().get<TransformComponent>(wheel).local[3] = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    scene.registry().get<PointLightComponent>(wheel).intensity = 3.0f;
    expect(authoring.create(scene, cart, "cart.prefab").success, "a prefab containing an instance should be created");
    PrefabDocument cartDocument;
    expect(loadPrefab(root / "cart.prefab", cartDocument).success && cartDocument.entities.size() == 1 &&
               cartDocument.nested.size() == 1 && cartDocument.nested.front().source == "parts/wheel.prefab",
           "an instance inside the captured subtree should become a nested prefab record");
    expect(scene.registry().get<PrefabInstanceComponent>(wheel).nestedId == cartDocument.nested.front().id,
           "the nested instance should remember its record");

    Scene other;
    Entity cartInstance = entt::null;
    expect(authoring.instantiate(other, "cart.prefab", entt::null, cartInstance).success,
           "a prefab with a nested prefab should instantiate");
    const Entity nestedWheel = childNamed(other, cartInstance, "Wheel");
    expect(nestedWheel != entt::null && other.registry().all_of<PrefabInstanceComponent>(nestedWheel) &&
               other.registry().get<PointLightComponent>(nestedWheel).intensity == 3.0f &&
               other.registry().get<TransformComponent>(nestedWheel).local[3].x == 1.0f,
           "the nested instance should carry the record's overrides and placement");
    expect(prefabInstanceRoot(other, nestedWheel) == nestedWheel &&
               prefabInstanceMembers(other, cartInstance).size() == 1,
           "nested instances should be separate instances");

    // A wheel instance holding a cart instance would contain itself.
    Entity looseWheel = entt::null;
    expect(authoring.instantiate(other, "parts/wheel.prefab", entt::null, looseWheel).success,
           "the cycle fixture wheel should instantiate");
    Entity innerCart = entt::null;
    expect(authoring.instantiate(other, "cart.prefab", looseWheel, innerCart).success,
           "the cycle fixture cart should instantiate");
    const std::string wheelBefore = readText(root / "parts/wheel.prefab");
    const PrefabResult cyclic = authoring.apply(other, looseWheel);
    expect(!cyclic && cyclic.error.find("cycle") != std::string::npos &&
               readText(root / "parts/wheel.prefab") == wheelBefore,
           "applying a nested prefab cycle should fail without writing the prefab");

    PrefabDocument selfNested;
    selfNested.entities.push_back({{1}, "Self", std::nullopt, glm::mat4(1.0f)});
    selfNested.nested.push_back({1, "self.prefab", std::nullopt, {}});
    expect(savePrefab(root / "self.prefab", selfNested).success, "the self-nesting fixture should save");
    Scene cycleScene;
    expect(!PrefabSystem(assets, options).instantiate("self.prefab", cycleScene) &&
               cycleScene.registry().view<const SceneEntityIdComponent>().empty(),
           "instantiating a nested cycle should fail without changing the scene");
}

// A refresh that fails after rebuilding the containing instance (here in a
// nested instance whose root no longer matches its prefab, which the isolated
// validation build cannot see) leaves the whole scene as it was.
void testFailedRefreshRestoresScene() {
    const std::filesystem::path root = freshDirectory("rollback");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root};
    PrefabAuthoring authoring(assets, options);
    Scene scene;
    const Entity wheel = scene.createEntity("Wheel");
    scene.registry().emplace<PointLightComponent>(wheel);
    expect(authoring.create(scene, wheel, "parts/wheel.prefab").success, "rollback: the inner prefab is created");
    const Entity cart = scene.createEntity("Cart");
    scene.setParent(wheel, cart);
    scene.setParent(scene.createEntity("Seat"), cart);
    expect(authoring.create(scene, cart, "cart.prefab").success, "rollback: the cart prefab is created");

    Scene level;
    level.createEntity("Unrelated");
    Entity instance = entt::null;
    expect(authoring.instantiate(level, "cart.prefab", entt::null, instance).success,
           "rollback: the cart instantiates");
    // The rebuild resets this name before the nested instance fails.
    level.registry().get<NameComponent>(childNamed(level, instance, "Seat")).name = "Renamed seat";
    level.setParent(level.createEntity("Added"), instance);
    level.registry().get<PrefabEntityComponent>(childNamed(level, instance, "Wheel")).entity.value += 100;
    std::string before;
    expect(SceneDocument::serialize(level, assets, options, before).success, "rollback: the level serializes");

    const PrefabInstanceResult refreshed =
        PrefabSystem(assets, options).refresh(level, instance, PrefabRefreshMode::RemoveAddedObjects);
    std::string after;
    expect(SceneDocument::serialize(level, assets, options, after).success, "rollback: the level still serializes");
    expect(!refreshed && refreshed.error.find("no longer matches") != std::string::npos &&
               refreshed.error.find("could not be restored") == std::string::npos,
           "rollback: the refresh fails in the nested instance");
    expect(after == before, "a refresh failing after its container was rebuilt should leave the scene unchanged");
}

void testApplyPropagation() {
    const std::filesystem::path root = freshDirectory("propagation");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root};
    PrefabAuthoring authoring(assets, options);
    Scene authored;
    const Entity wheel = authored.createEntity("Wheel");
    authored.registry().emplace<PointLightComponent>(wheel);
    expect(authoring.create(authored, wheel, "wheel.prefab").success, "the propagation wheel should be created");
    const Entity cart = authored.createEntity("Cart");
    const Entity axle = authored.createEntity("Axle");
    authored.setParent(axle, cart);
    authored.setParent(wheel, cart);
    expect(authoring.create(authored, cart, "cart.prefab").success, "the propagation cart should be created");

    Scene scene;
    Entity first = entt::null;
    Entity second = entt::null;
    expect(authoring.instantiate(scene, "cart.prefab", entt::null, first).success &&
               authoring.instantiate(scene, "cart.prefab", entt::null, second).success,
           "two carts should instantiate");
    const SceneEntityId secondId = scene.id(second);
    scene.registry().get<PointLightComponent>(childNamed(scene, first, "Wheel")).intensity = 8.0f;
    scene.destroyEntity(childNamed(scene, first, "Axle"));
    scene.setParent(scene.createEntity("Seat"), first);
    const PrefabResult applied = authoring.apply(scene, first);
    expect(applied.success, "applying the first cart should succeed");

    second = scene.findEntity(secondId);
    const Entity secondWheel = childNamed(scene, second, "Wheel");
    expect(secondWheel != entt::null && scene.registry().get<PointLightComponent>(secondWheel).intensity == 8.0f,
           "apply should push a changed nested override into other instances' nested instances");
    expect(childNamed(scene, second, "Axle") == entt::null && childNamed(scene, second, "Seat") != entt::null &&
               scene.registry().get<RelationshipComponent>(second).children.size() == 2,
           "apply should remove deleted entities from and add new entities to the other instances");
    expect(!authoring.create(scene, childNamed(scene, second, "Seat"), "seat.prefab"),
           "an entity inside an instance should not become a prefab of its own");
    std::vector<PrefabOverride> remaining;
    expect(PrefabSystem(assets, options).overrides(scene, second, remaining).success && remaining.empty(),
           "the refreshed instance should match the applied prefab");
}

SceneCollectionId addCollection(Scene& scene, const std::string& name) {
    SceneCollections collections = scene.collections();
    SceneCollectionId id;
    expect(collections.create(name, {}, id).success, "the collection should be created");
    scene.setCollections(std::move(collections));
    return id;
}

void joinCollection(Scene& scene, Entity entity, SceneCollectionId collection) {
    scene.registry().get_or_emplace<CollectionMembershipComponent>(entity).insert(collection);
}

bool inCollection(const Scene& scene, Entity entity, SceneCollectionId collection) {
    const auto* membership = scene.registry().try_get<CollectionMembershipComponent>(entity);
    return membership != nullptr && membership->contains(collection);
}

void testCollectionsStayInScene() {
    const std::filesystem::path root = freshDirectory("collections");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root, .componentTypes = moduleComponentTypes()};
    PrefabAuthoring authoring(assets, options);
    Scene scene;
    const SceneCollectionId props = addCollection(scene, "Props");
    const Entity lamp = authorLamp(scene);
    const Entity bulb = childNamed(scene, lamp, "Bulb");
    joinCollection(scene, lamp, props);
    joinCollection(scene, bulb, props);
    scene.registry().emplace<CollectionInstanceComponent>(bulb, props);
    expect(authoring.create(scene, lamp, "lamp.prefab").success, "a prefab should be made from collection members");
    const std::string text = readText(root / "lamp.prefab");
    expect(text.find("collection_membership") == std::string::npos &&
               text.find("collection_instance") == std::string::npos,
           "a prefab should not store scene collection links");
    expect(inCollection(scene, lamp, props) && inCollection(scene, bulb, props),
           "creating a prefab should keep the source entities in their collections");

    Scene empty;
    Entity elsewhere = entt::null;
    expect(authoring.instantiate(empty, "lamp.prefab", entt::null, elsewhere).success &&
               !empty.registry().all_of<CollectionMembershipComponent>(elsewhere),
           "a prefab made from collection members should instantiate into a scene without that collection");

    // A prefab written before collections were scene-local still loads.
    PrefabDocument legacy;
    expect(loadPrefab(root / "lamp.prefab", legacy).success, "the prefab should load");
    legacy.entities.front().components.emplace_back("collection_membership", R"({"collections":[77]})");
    expect(savePrefab(root / "legacy.prefab", legacy).success, "the legacy fixture should save");
    Entity legacyInstance = entt::null;
    expect(authoring.instantiate(empty, "legacy.prefab", entt::null, legacyInstance).success,
           "a prefab holding collection ids should instantiate into a scene without them");

    Entity second = entt::null;
    expect(authoring.instantiate(scene, "lamp.prefab", entt::null, second).success, "a second lamp should instantiate");
    const SceneEntityId secondId = scene.id(second);
    const SceneEntityId secondBulbId = scene.id(childNamed(scene, second, "Bulb"));
    const SceneCollectionId lights = addCollection(scene, "Lights");
    joinCollection(scene, second, lights);
    joinCollection(scene, scene.findEntity(secondBulbId), props);
    PrefabEntityOverrides overrides;
    expect(authoring.overrides(scene, second, overrides).success && overrides.components.empty(),
           "collection membership should never be an override");

    scene.registry().get<PointLightComponent>(lamp).intensity = 42.0f;
    expect(authoring.apply(scene, lamp).success, "the first lamp should apply");
    expect(scene.registry().get<PointLightComponent>(scene.findEntity(secondId)).intensity == 42.0f &&
               inCollection(scene, scene.findEntity(secondId), lights) &&
               inCollection(scene, scene.findEntity(secondBulbId), props),
           "apply propagation should keep the other instance's collection links");
    expect(authoring.revert(scene, scene.findEntity(secondId)).success &&
               inCollection(scene, scene.findEntity(secondId), lights) &&
               inCollection(scene, scene.findEntity(secondBulbId), props),
           "revert should keep the instance's collection links");
    expect(readText(root / "lamp.prefab").find("collection_") == std::string::npos,
           "apply should not store collection links either");
}

FollowComponent& follow(Scene& scene, Entity entity) {
    return scene.registry().get<FollowComponent>(entity);
}

void testEntityReferenceRemap() {
    OperatorFixture fixture(freshDirectory("references"));
    Scene& scene = fixture.scene;
    const Entity outside = scene.createEntity("Outside");
    const Entity rig = scene.createEntity("Rig");
    const Entity arm = scene.createEntity("Arm");
    scene.setParent(arm, rig);
    scene.registry().emplace<FollowComponent>(
        rig, FollowComponent{.target = scene.id(arm), .settings = {.target = scene.id(rig), .distance = 2.0f}});
    scene.registry().emplace<FollowComponent>(arm, FollowComponent{.target = scene.id(outside)});
    const SceneEntityId outsideId = scene.id(outside);
    const SceneEntityId rigId = scene.id(rig);

    // Duplicate: references inside the copied set follow the copies.
    fixture.select(rig);
    expect(fixture.invoke("object.duplicate").status == OperatorStatus::Finished, "the rig should duplicate");
    const Entity copy = fixture.find(*fixture.selection.active());
    const Entity copyArm = childNamed(scene, copy, "Arm");
    expect(copy != entt::null && copy != fixture.find(rigId) && follow(scene, copy).target == scene.id(copyArm) &&
               follow(scene, copy).settings.target == scene.id(copy),
           "duplicating should remap references (including nested struct fields) to the copies");
    expect(follow(scene, copyArm).target == outsideId, "duplicating should keep references to outside entities");

    // Copy and paste take the same path.
    std::string fragment;
    std::vector<SceneEntityId> pasted;
    const SceneEntityId pastedSource[]{rigId};
    expect(SceneFragment::copy(scene, fixture.assets, fixture.document.options(), pastedSource, fragment).success &&
               SceneFragment::paste(fragment, scene, fixture.assets, fixture.document.options(), entt::null, pasted)
                   .success &&
               pasted.size() == 1,
           "the rig should copy and paste");
    const Entity pastedRig = fixture.find(pasted.front());
    expect(follow(scene, pastedRig).target == scene.id(childNamed(scene, pastedRig, "Arm")) &&
               follow(scene, pastedRig).settings.target == pasted.front(),
           "pasting should remap references to the pasted entities");

    // Prefabs name only their own entities.
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    const PrefabResult created = authoring.create(scene, fixture.find(rigId), "rig.prefab");
    expect(created.success && created.diagnostics.size() == 1 &&
               created.diagnostics.front().find("outside the prefab") != std::string::npos,
           "creating a prefab should report a reference outside it");
    Scene other;
    Entity instance = entt::null;
    expect(authoring.instantiate(other, "rig.prefab", entt::null, instance).success, "the rig should instantiate");
    const Entity instanceArm = childNamed(other, instance, "Arm");
    expect(follow(other, instance).target == other.id(instanceArm) &&
               follow(other, instance).settings.target == other.id(instance),
           "instantiating should point references at the instance's own entities");
    expect(!follow(other, instanceArm).target, "a reference outside the prefab should be stored as none");
    std::vector<PrefabOverride> overrides;
    PrefabSystem prefabs(fixture.assets, fixture.document.options());
    expect(prefabs.overrides(other, instance, overrides).success && overrides.empty(),
           "remapped references should not count as overrides");

    // Refresh keeps them pointing inside the refreshed instance.
    Entity second = entt::null;
    expect(authoring.instantiate(other, "rig.prefab", entt::null, second).success, "a second rig should instantiate");
    const SceneEntityId secondId = other.id(second);
    other.registry().get<NameComponent>(instance).name = "Renamed rig";
    expect(authoring.apply(other, instance).success, "the renamed rig should apply");
    const Entity refreshed = other.findEntity(secondId);
    expect(other.registry().get<NameComponent>(refreshed).name == "Renamed rig" &&
               follow(other, refreshed).target == other.id(childNamed(other, refreshed, "Arm")) &&
               follow(other, refreshed).settings.target == secondId,
           "refreshing should keep references inside each instance");

    // An override pointing outside the instance keeps its target through a
    // refresh, even when its scene id equals a prefab-local id.
    const Entity far = other.createEntity("Far");
    std::vector<PrefabOverride> secondOverrides;
    follow(other, refreshed).target = other.id(far);
    expect(prefabs.overrides(other, refreshed, secondOverrides).success && secondOverrides.size() == 1 &&
               prefabs.refresh(other, refreshed, secondOverrides, PrefabRefreshMode::KeepAddedObjects).success,
           "an outside reference should be an override the instance refreshes with");
    expect(follow(other, other.findEntity(secondId)).target == other.id(far),
           "a refreshed override should keep referencing the entity outside the instance");

    // A reference to a prefab entity the instance deleted becomes none.
    expect(prefabs.refresh(other, other.findEntity(secondId), {}, PrefabRefreshMode::RemoveAddedObjects).success &&
               follow(other, other.findEntity(secondId)).target ==
                   other.id(childNamed(other, other.findEntity(secondId), "Arm")),
           "reverting should point the reference at the instance's arm again");
    const Entity secondArm = childNamed(other, other.findEntity(secondId), "Arm");
    expect(recordPrefabEntityRemoval(other, secondArm), "the arm's deletion should be recorded");
    other.destroyEntity(secondArm);
    expect(prefabs.refresh(other, other.findEntity(secondId), PrefabRefreshMode::KeepAddedObjects).success &&
               childNamed(other, other.findEntity(secondId), "Arm") == entt::null &&
               !follow(other, other.findEntity(secondId)).target,
           "a reference to a removed prefab entity should become none, not another entity's id");
}

Entity namedChild(const Scene& scene, SceneEntityId parent, const std::string& name) {
    return childNamed(scene, scene.findEntity(parent), name);
}

void testStructuralOverrides() {
    OperatorFixture fixture(freshDirectory("structure"));
    Scene& scene = fixture.scene;
    const Entity cart = scene.createEntity("Cart");
    const Entity axle = scene.createEntity("Axle");
    const Entity wheel = scene.createEntity("Wheel");
    const Entity seat = scene.createEntity("Seat");
    const Entity lamp = scene.createEntity("Lamp");
    scene.setParent(axle, cart);
    scene.setParent(wheel, axle);
    scene.setParent(seat, cart);
    scene.setParent(lamp, cart);
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    expect(authoring.create(scene, cart, "cart.prefab").success, "the cart prefab should be created");
    const SceneEntityId firstId = scene.id(cart);
    Entity secondRoot = entt::null;
    expect(authoring.instantiate(scene, "cart.prefab", entt::null, secondRoot).success,
           "a second cart should instantiate");
    const SceneEntityId secondId = scene.id(secondRoot);

    // Delete the first cart's axle (and its wheel) and move its seat under the lamp.
    fixture.select(namedChild(scene, firstId, "Axle"));
    expect(fixture.invoke("object.delete").status == OperatorStatus::Finished, "the axle should delete");
    const SceneEntityId lampId = scene.id(namedChild(scene, firstId, "Lamp"));
    const SceneEntityId seatId = scene.id(namedChild(scene, firstId, "Seat"));
    fixture.edit([&] { scene.reparent(fixture.find(seatId), fixture.find(lampId)); });
    std::vector<PrefabOverride> live;
    PrefabSystem prefabs(fixture.assets, fixture.document.options());
    expect(prefabs.overrides(scene, fixture.find(firstId), live).success, "the overrides should be computed");
    const auto has = [&live](const std::string& field) {
        return std::ranges::any_of(live, [&field](const PrefabOverride& value) { return value.field == field; });
    };
    expect(has("/removed") && has("/parent"), "deleting and reparenting should be recorded as overrides");
    PrefabEntityOverrides seatOverrides;
    expect(authoring.overrides(scene, fixture.find(seatId), seatOverrides).success && seatOverrides.parent &&
               seatOverrides.removedEntities == 1,
           "the override query should report the moved entity and the deleted one");

    // A refresh (revertField on another value) keeps both.
    fixture.edit([&] { scene.registry().get<NameComponent>(fixture.find(lampId)).name = "Torch"; });
    expect(authoring.revertField(scene, fixture.find(lampId), "", "/name").success,
           "reverting the lamp's name should refresh the instance");
    expect(namedChild(scene, firstId, "Axle") == entt::null &&
               scene.registry().get<RelationshipComponent>(fixture.find(seatId)).parent == fixture.find(lampId),
           "a refresh should keep the deletion and the move");

    // Survives a save and load of the scene.
    std::string saved;
    Scene reloaded;
    expect(SceneDocument::serialize(scene, fixture.assets, fixture.document.options(), saved).success &&
               SceneDocument::deserialize(saved, fixture.assets, fixture.document.options(), reloaded).success,
           "the scene with structural overrides should save and load");
    expect(prefabs.refresh(reloaded, reloaded.findEntity(firstId), PrefabRefreshMode::KeepAddedObjects).success &&
               namedChild(reloaded, firstId, "Axle") == entt::null,
           "a reloaded instance should keep its deletion through a refresh");

    // Another instance's apply keeps them too.
    fixture.edit([&] {
        const Entity secondWheel = childNamed(scene, namedChild(scene, secondId, "Axle"), "Wheel");
        scene.registry().get<NameComponent>(secondWheel).name = "Rim";
    });
    expect(authoring.apply(scene, fixture.find(secondId)).success, "the second cart should apply");
    expect(namedChild(scene, firstId, "Axle") == entt::null &&
               scene.registry().get<RelationshipComponent>(fixture.find(seatId)).parent == fixture.find(lampId),
           "apply propagation should keep the other instance's deletion and move");

    // Revert restores; undo brings the overrides back.
    fixture.select(fixture.find(firstId));
    expect(fixture.invoke("prefab.revert").status == OperatorStatus::Finished, "the first cart should revert");
    const Entity restoredAxle = namedChild(scene, firstId, "Axle");
    expect(restoredAxle != entt::null && childNamed(scene, restoredAxle, "Rim") != entt::null &&
               scene.registry().get<RelationshipComponent>(fixture.find(seatId)).parent == fixture.find(firstId),
           "revert should restore the deleted entities and the parent");
    fixture.editing().undo();
    expect(namedChild(scene, firstId, "Axle") == entt::null, "undoing revert should delete them again");

    // Apply writes both changes into the prefab and the other instance.
    fixture.select(fixture.find(firstId));
    expect(fixture.invoke("prefab.apply").status == OperatorStatus::Finished, "the first cart should apply");
    PrefabDocument document;
    expect(loadPrefab(fixture.root / "cart.prefab", document).success &&
               std::ranges::none_of(
                   document.entities,
                   [](const PrefabEntity& entity) { return entity.name == "Axle" || entity.name == "Rim"; }),
           "apply should remove deleted entities from the prefab");
    const Entity secondLamp = namedChild(scene, secondId, "Lamp");
    expect(namedChild(scene, secondId, "Axle") == entt::null && childNamed(scene, secondLamp, "Seat") != entt::null,
           "apply should carry the deletion and the move to the other instance");
    std::vector<PrefabOverride> after;
    expect(prefabs.overrides(scene, fixture.find(firstId), after).success && after.empty(),
           "the applied instance should have no structural overrides left");
}

bool hasNestedRemoval(const std::vector<PrefabOverride>& overrides) {
    return std::ranges::any_of(
        overrides, [](const PrefabOverride& value) { return value.nested != 0 && value.field == "/removed"; });
}

const std::vector<PrefabOverride>& persistedOverrides(const Scene& scene, Entity root) {
    return scene.registry().get<PrefabInstanceComponent>(root).overrides;
}

// Refreshes an instance with its live overrides, as revertField and apply
// propagation do.
bool refreshWithLiveOverrides(PrefabSystem& prefabs, Scene& scene, Entity root) {
    std::vector<PrefabOverride> live;
    return prefabs.overrides(scene, root, live).success &&
           prefabs.refresh(scene, root, live, PrefabRefreshMode::KeepAddedObjects).success;
}

void testNestedInstanceRemoval() {
    OperatorFixture fixture(freshDirectory("nested-removal"));
    Scene& scene = fixture.scene;
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    PrefabSystem prefabs(fixture.assets, fixture.document.options());
    const Entity wheel = scene.createEntity("Wheel");
    scene.registry().emplace<PointLightComponent>(wheel);
    expect(authoring.create(scene, wheel, "wheel.prefab").success, "the wheel prefab should be created");
    const Entity cart = scene.createEntity("Cart");
    scene.setParent(wheel, cart);
    expect(authoring.create(scene, cart, "cart.prefab").success, "the cart prefab should be created");
    const SceneEntityId firstId = scene.id(cart);
    Entity second = entt::null;
    Entity third = entt::null;
    expect(authoring.instantiate(scene, "cart.prefab", entt::null, second).success &&
               authoring.instantiate(scene, "cart.prefab", entt::null, third).success,
           "more carts should instantiate");
    const SceneEntityId secondId = scene.id(second);
    const SceneEntityId thirdId = scene.id(third);

    // Moving the third cart's wheel out of it removes it from the cart and
    // keeps it as an instance of its own prefab.
    const SceneEntityId movedWheelId = scene.id(namedChild(scene, thirdId, "Wheel"));
    fixture.select(fixture.find(movedWheelId));
    expect(fixture.invoke("outliner.parent_drop", R"({"parent":null})").status == OperatorStatus::Finished,
           "the third cart's wheel should move to the scene root");
    const Entity movedWheel = fixture.find(movedWheelId);
    expect(scene.registry().get<RelationshipComponent>(movedWheel).parent == entt::null &&
               scene.registry().get<PrefabInstanceComponent>(movedWheel).nestedId == 0 &&
               hasNestedRemoval(persistedOverrides(scene, fixture.find(thirdId))),
           "a nested instance moved out should become a placed instance and be recorded as removed");
    expect(refreshWithLiveOverrides(prefabs, scene, fixture.find(thirdId)) &&
               namedChild(scene, thirdId, "Wheel") == entt::null && fixture.find(movedWheelId) != entt::null,
           "a refresh should not recreate a nested instance moved out");

    // Delete the first cart's nested wheel.
    fixture.select(namedChild(scene, firstId, "Wheel"));
    expect(fixture.invoke("object.delete").status == OperatorStatus::Finished, "the nested wheel should delete");
    expect(hasNestedRemoval(persistedOverrides(scene, fixture.find(firstId))),
           "deleting a nested instance should be recorded on the containing instance");
    std::vector<PrefabOverride> live;
    expect(prefabs.overrides(scene, fixture.find(firstId), live).success && live.size() == 1 && hasNestedRemoval(live),
           "the removed nested record should be a live override");
    PrefabEntityOverrides header;
    expect(authoring.overrides(scene, fixture.find(firstId), header).success && header.removedEntities == 1,
           "the instance header should count the deleted nested instance");
    expect(refreshWithLiveOverrides(prefabs, scene, fixture.find(firstId)) &&
               prefabs.refresh(scene, fixture.find(firstId), PrefabRefreshMode::KeepAddedObjects).success &&
               namedChild(scene, firstId, "Wheel") == entt::null,
           "a refresh should not recreate the deleted nested instance");

    // Survives a save and load of the scene.
    std::string saved;
    Scene reloaded;
    expect(SceneDocument::serialize(scene, fixture.assets, fixture.document.options(), saved).success &&
               SceneDocument::deserialize(saved, fixture.assets, fixture.document.options(), reloaded).success &&
               prefabs.refresh(reloaded, reloaded.findEntity(firstId), PrefabRefreshMode::KeepAddedObjects).success &&
               namedChild(reloaded, firstId, "Wheel") == entt::null,
           "a reloaded instance should keep the nested instance deleted");

    // Another instance's apply keeps it deleted.
    fixture.edit(
        [&] { scene.registry().get<PointLightComponent>(namedChild(scene, secondId, "Wheel")).intensity = 6.0f; });
    expect(authoring.apply(scene, fixture.find(secondId)).success, "the second cart should apply");
    expect(namedChild(scene, firstId, "Wheel") == entt::null && namedChild(scene, thirdId, "Wheel") == entt::null,
           "apply propagation should keep the other instances' nested deletions");

    // Revert restores it; undo deletes it again.
    fixture.select(fixture.find(firstId));
    expect(fixture.invoke("prefab.revert").status == OperatorStatus::Finished, "the first cart should revert");
    const Entity restored = namedChild(scene, firstId, "Wheel");
    expect(restored != entt::null && scene.registry().get<PointLightComponent>(restored).intensity == 6.0f,
           "revert should recreate the nested instance from the current record");
    fixture.editing().undo();
    expect(namedChild(scene, firstId, "Wheel") == entt::null, "undoing revert should delete it again");

    // Apply removes the record from the prefab and from the other instance.
    fixture.select(fixture.find(firstId));
    expect(fixture.invoke("prefab.apply").status == OperatorStatus::Finished, "the first cart should apply");
    PrefabDocument document;
    expect(loadPrefab(fixture.root / "cart.prefab", document).success && document.nested.empty(),
           "apply should remove the deleted nested record from the prefab");
    expect(namedChild(scene, secondId, "Wheel") == entt::null && fixture.find(movedWheelId) != entt::null,
           "apply should remove the nested instance from the other instance and keep the one moved out");
    std::vector<PrefabOverride> after;
    expect(prefabs.overrides(scene, fixture.find(firstId), after).success && after.empty(),
           "the applied instance should have no overrides left");
}

void testMovesOutOfAndWithinInstances() {
    OperatorFixture fixture(freshDirectory("moves"));
    Scene& scene = fixture.scene;
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    PrefabSystem prefabs(fixture.assets, fixture.document.options());
    const Entity cart = scene.createEntity("Cart");
    const Entity seat = scene.createEntity("Seat");
    const Entity lamp = scene.createEntity("Lamp");
    const Entity bulb = scene.createEntity("Bulb");
    scene.registry().emplace<PointLightComponent>(lamp, glm::vec3(1.0f), 3.0f, 4.0f);
    scene.setParent(seat, cart);
    scene.setParent(lamp, cart);
    scene.setParent(bulb, lamp);
    expect(authoring.create(scene, cart, "cart.prefab").success, "the cart prefab should be created");
    const SceneEntityId firstId = scene.id(cart);
    Entity second = entt::null;
    expect(authoring.instantiate(scene, "cart.prefab", entt::null, second).success, "a second cart should instantiate");
    const SceneEntityId secondId = scene.id(second);
    const SceneEntityId seatId = scene.id(seat);
    const SceneEntityId lampId = scene.id(lamp);
    const SceneEntityId bulbId = scene.id(bulb);

    // Move the seat under an object added to the first cart.
    SceneEntityId rackId;
    fixture.edit([&] {
        const Entity rack = scene.createEntity("Rack");
        scene.setParent(rack, fixture.find(firstId));
        rackId = scene.id(rack);
    });
    fixture.select(fixture.find(seatId));
    expect(fixture.invoke("outliner.parent_drop", R"({"parent":)" + std::to_string(rackId.value) + "}").status ==
               OperatorStatus::Finished,
           "the seat should move under the added rack");
    std::vector<PrefabOverride> live;
    expect(prefabs.overrides(scene, fixture.find(firstId), live).success && live.size() == 1 &&
               live.front().field == "/parent" &&
               nlohmann::json::parse(live.front().jsonValue).get<uint64_t>() == (rackId.value | (uint64_t{1} << 63u)),
           "moving under an added object should be a /parent override naming it by tagged scene id");
    PrefabEntityOverrides seatOverrides;
    expect(authoring.overrides(scene, fixture.find(seatId), seatOverrides).success && seatOverrides.parent,
           "the override query should report the move");
    const auto seatParent = [&] { return scene.registry().get<RelationshipComponent>(fixture.find(seatId)).parent; };

    // A refresh keeps it, also after a save and load.
    fixture.edit([&] { scene.registry().get<NameComponent>(fixture.find(lampId)).name = "Torch"; });
    expect(authoring.revertField(scene, fixture.find(lampId), "", "/name").success &&
               seatParent() == fixture.find(rackId),
           "a refresh should keep the seat under the added rack");
    std::string saved;
    Scene reloaded;
    expect(SceneDocument::serialize(scene, fixture.assets, fixture.document.options(), saved).success &&
               SceneDocument::deserialize(saved, fixture.assets, fixture.document.options(), reloaded).success &&
               refreshWithLiveOverrides(prefabs, reloaded, reloaded.findEntity(firstId)) &&
               reloaded.registry().get<RelationshipComponent>(reloaded.findEntity(seatId)).parent ==
                   reloaded.findEntity(rackId),
           "a reloaded instance should keep the seat under the rack through a refresh");

    // Moving the lamp to the scene root takes it (and its bulb) out of the
    // instance as plain objects that keep their components.
    fixture.select(fixture.find(lampId));
    expect(fixture.invoke("outliner.parent_drop", R"({"parent":null})").status == OperatorStatus::Finished,
           "the lamp should move to the scene root");
    const auto unlinkedLamp = [&] {
        const Entity moved = fixture.find(lampId);
        return moved != entt::null && scene.registry().get<RelationshipComponent>(moved).parent == entt::null &&
               !scene.registry().all_of<PrefabEntityComponent>(moved) &&
               !scene.registry().all_of<PrefabEntityComponent>(fixture.find(bulbId)) &&
               scene.registry().get<PointLightComponent>(moved).intensity == 3.0f;
    };
    expect(unlinkedLamp(), "the moved lamp should be an unlinked scene object with its components");
    expect(prefabs.overrides(scene, fixture.find(firstId), live).success &&
               std::ranges::any_of(live, [](const PrefabOverride& value) { return value.field == "/removed"; }),
           "moving out should be recorded as a removal");
    expect(refreshWithLiveOverrides(prefabs, scene, fixture.find(firstId)) &&
               namedChild(scene, firstId, "Lamp") == entt::null && unlinkedLamp() &&
               seatParent() == fixture.find(rackId),
           "a refresh should neither recreate the lamp in the instance nor touch the moved one");
    fixture.editing().undo();
    expect(scene.registry().get<RelationshipComponent>(fixture.find(lampId)).parent == fixture.find(firstId) &&
               scene.registry().all_of<PrefabEntityComponent>(fixture.find(lampId)),
           "undoing the move should put the lamp back, linked");
    fixture.editing().redo();
    expect(unlinkedLamp(), "redoing the move should unlink it again");

    // Revert restores both; undo brings the overrides back.
    fixture.select(fixture.find(firstId));
    expect(fixture.invoke("prefab.revert").status == OperatorStatus::Finished, "the first cart should revert");
    expect(seatParent() == fixture.find(firstId) && fixture.find(rackId) == entt::null &&
               namedChild(scene, firstId, "Lamp") != entt::null && unlinkedLamp(),
           "revert should restore the seat's parent and a lamp, and keep the moved lamp");
    fixture.editing().undo();
    expect(seatParent() == fixture.find(rackId) && namedChild(scene, firstId, "Lamp") == entt::null,
           "undoing revert should restore the moves");

    // Apply writes both into the prefab and the other instance.
    fixture.select(fixture.find(firstId));
    expect(fixture.invoke("prefab.apply").status == OperatorStatus::Finished, "the first cart should apply");
    PrefabDocument document;
    expect(
        loadPrefab(fixture.root / "cart.prefab", document).success &&
            std::ranges::none_of(document.entities, [](const PrefabEntity& entity) { return entity.name == "Lamp"; }),
        "apply should remove the moved-out lamp from the prefab");
    const Entity secondRack = namedChild(scene, secondId, "Rack");
    expect(secondRack != entt::null && childNamed(scene, secondRack, "Seat") != entt::null &&
               namedChild(scene, secondId, "Lamp") == entt::null && unlinkedLamp(),
           "apply should give the other instance the rack with the seat and remove its lamp");
    expect(prefabs.overrides(scene, fixture.find(firstId), live).success && live.empty(),
           "the applied instance should have no overrides left");

    // Moving into another instance makes the object an added one there.
    fixture.select(fixture.find(seatId));
    expect(fixture.invoke("outliner.parent_drop", R"({"parent":)" + std::to_string(secondId.value) + "}").status ==
                   OperatorStatus::Finished &&
               !scene.registry().all_of<PrefabEntityComponent>(fixture.find(seatId)),
           "a prefab object moved into another instance should be added there, unlinked");
    PrefabEntityOverrides movedOverrides;
    expect(authoring.overrides(scene, fixture.find(seatId), movedOverrides).success && movedOverrides.added &&
               movedOverrides.instanceRoot == secondId,
           "the moved object should count as added to the other instance");
}

// R > Q > P, then P under an added A and Q under P: falling back to prefab
// parents (as the validation rebuild does) must not make a cycle.
void testMovesUnderAddedWithoutCycles() {
    OperatorFixture fixture(freshDirectory("move-cycles"));
    Scene& scene = fixture.scene;
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    PrefabSystem prefabs(fixture.assets, fixture.document.options());
    const Entity root = scene.createEntity("R");
    const Entity q = scene.createEntity("Q");
    const Entity p = scene.createEntity("P");
    scene.setParent(q, root);
    scene.setParent(p, q);
    expect(authoring.create(scene, root, "chain.prefab").success, "the chain prefab should be created");
    const Entity added = scene.createEntity("A");
    scene.setParent(added, root);
    scene.reparent(p, added);
    scene.reparent(q, p);
    const SceneEntityId rootId = scene.id(root);
    const SceneEntityId pId = scene.id(p);
    const SceneEntityId qId = scene.id(q);
    const SceneEntityId addedId = scene.id(added);
    const auto parentOf = [&](SceneEntityId id) {
        return scene.registry().get<RelationshipComponent>(scene.findEntity(id)).parent;
    };
    expect(refreshWithLiveOverrides(prefabs, scene, scene.findEntity(rootId)) &&
               parentOf(pId) == scene.findEntity(addedId) && parentOf(qId) == scene.findEntity(pId),
           "a refresh should keep both moves without making a cycle");
    std::vector<PrefabOverride> live;
    expect(prefabs.overrides(scene, scene.findEntity(rootId), live).success &&
               prefabs.refresh(scene, scene.findEntity(rootId), live, PrefabRefreshMode::RemoveAddedObjects).success &&
               scene.findEntity(addedId) == entt::null &&
               prefabInstanceRoot(scene, scene.findEntity(pId)) == scene.findEntity(rootId) &&
               prefabInstanceRoot(scene, scene.findEntity(qId)) == scene.findEntity(rootId),
           "removing the added parent should put the moved entities back without a cycle");
}

FollowComponent& followOf(Scene& scene, Entity entity) {
    return scene.registry().get<FollowComponent>(entity);
}

void testNestedReferences() {
    OperatorFixture fixture(freshDirectory("nested-references"));
    Scene& scene = fixture.scene;
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    // Scene ids well above the prefab-local ids, so the two cannot match.
    for (int index = 0; index < 20; ++index) {
        scene.createEntity("Padding");
    }
    const Entity wheel = scene.createEntity("Wheel");
    const Entity hub = scene.createEntity("Hub");
    scene.setParent(hub, wheel);
    expect(authoring.create(scene, wheel, "wheel.prefab").success, "the wheel prefab should be created");
    const Entity cart = scene.createEntity("Cart");
    const Entity axle = scene.createEntity("Axle");
    const Entity outside = scene.createEntity("Outside");
    scene.setParent(axle, cart);
    scene.setParent(wheel, cart);
    // The axle follows the wheel's hub (a reference into a nested instance);
    // the hub follows the axle (a nested instance referencing its container)
    // and, in its settings, an entity outside the cart.
    scene.registry().emplace<FollowComponent>(axle, FollowComponent{.target = scene.id(hub)});
    scene.registry().emplace<FollowComponent>(
        hub, FollowComponent{.target = scene.id(axle), .settings = {.target = scene.id(outside)}});
    const PrefabResult created = authoring.create(scene, cart, "cart.prefab");
    expect(created.success && std::ranges::any_of(created.diagnostics,
                                                  [](const std::string& diagnostic) {
                                                      return diagnostic.find("outside the prefab") != std::string::npos;
                                                  }),
           "the cart should be created, reporting the reference outside it");

    PrefabDocument document;
    expect(loadPrefab(fixture.root / "cart.prefab", document).success && document.nested.size() == 1,
           "the cart prefab should load with its nested wheel");
    const auto axleEntity = std::ranges::find(document.entities, std::string("Axle"),
                                              [](const PrefabEntity& entity) { return entity.name; });
    const auto follow = axleEntity == document.entities.end()
                            ? document.entities.front().components.end()
                            : std::ranges::find(axleEntity->components, std::string("test_follow"),
                                                &std::pair<std::string, std::string>::first);
    const uint64_t hubReference = prefabNestedReference(document.nested.front().id, PrefabEntityId{2});
    expect(axleEntity != document.entities.end() && follow != axleEntity->components.end() &&
               nlohmann::json::parse(follow->second).value("target", uint64_t{0}) == hubReference,
           "a reference into a nested instance should be stored as a nested reference");
    const auto hubFollow =
        std::ranges::find(document.nested.front().overrides, std::string("test_follow"), &PrefabOverride::component);
    const nlohmann::json hubValue = hubFollow == document.nested.front().overrides.end()
                                        ? nlohmann::json()
                                        : nlohmann::json::parse(hubFollow->jsonValue);
    expect(hubValue.is_object() && axleEntity != document.entities.end() &&
               hubValue.value("target", uint64_t{0}) == (axleEntity->id.value | (uint64_t{1} << 63u)) &&
               hubValue["settings"]["target"].is_null(),
           "a nested record's reference to the container should name its prefab-local id, and others none");
    const std::string text = readText(fixture.root / "cart.prefab");
    for (const Entity entity : {axle, outside, cart}) {
        expect(text.find(std::to_string(scene.id(entity).value | (uint64_t{1} << 63u))) == std::string::npos,
               "the prefab file should hold no scene ids");
    }

    // Instances resolve both kinds to their own entities.
    Scene other;
    for (int index = 0; index < 50; ++index) {
        other.createEntity("Padding");
    }
    Entity first = entt::null;
    Entity second = entt::null;
    expect(authoring.instantiate(other, "cart.prefab", entt::null, first).success &&
               authoring.instantiate(other, "cart.prefab", entt::null, second).success,
           "two carts should instantiate");
    const auto references = [&](Entity root) {
        const Entity instanceAxle = childNamed(other, root, "Beam") != entt::null ? childNamed(other, root, "Beam")
                                                                                  : childNamed(other, root, "Axle");
        const Entity instanceWheel = childNamed(other, root, "Wheel");
        const Entity instanceHub = instanceWheel == entt::null ? entt::null : childNamed(other, instanceWheel, "Hub");
        return instanceAxle != entt::null && instanceHub != entt::null &&
               followOf(other, instanceAxle).target == other.id(instanceHub) &&
               followOf(other, instanceHub).target == other.id(instanceAxle) &&
               !followOf(other, instanceHub).settings.target;
    };
    expect(references(first) && references(second), "instances should resolve references into and out of nested ones");
    PrefabSystem prefabs(fixture.assets, fixture.document.options());
    std::vector<PrefabOverride> overrides;
    expect(prefabs.overrides(other, first, overrides).success && overrides.empty(),
           "resolved nested references should not be overrides");

    // Apply keeps them in the other instance.
    const SceneEntityId secondId = other.id(second);
    other.registry().get<NameComponent>(childNamed(other, first, "Axle")).name = "Beam";
    expect(authoring.apply(other, first).success, "the renamed cart should apply");
    expect(references(first) && references(other.findEntity(secondId)) &&
               childNamed(other, other.findEntity(secondId), "Beam") != entt::null,
           "apply and refresh should keep the references inside each instance");
    const std::string applied = readText(fixture.root / "cart.prefab");
    for (const Entity entity : prefabInstanceMembers(other, first)) {
        expect(applied.find(std::to_string(other.id(entity).value | (uint64_t{1} << 63u))) == std::string::npos,
               "an applied prefab should hold no scene ids");
    }
}

// Opens reference into an empty scene and saves it again without edits;
// returns whether the file kept every byte.
bool reopenKeepsBytes(const std::filesystem::path& root, AssetManager& assets, const SceneDocumentOptions& options,
                      const std::filesystem::path& reference) {
    const std::string written = readText(root / reference);
    PrefabSystem prefabs(assets, options);
    Scene document;
    OpenedPrefabDocument opened;
    SavedPrefabDocument saved;
    const bool roundTrip = openPrefabDocument(reference, document, prefabs, opened).success &&
                           opened.fileBytes == written &&
                           savePrefabDocument(document, opened.root, reference, prefabs, saved).success &&
                           saved.before == written && saved.after == written;
    return roundTrip && readText(root / reference) == written;
}

void testOpenAndSaveRoundTrip() {
    OperatorFixture fixture(freshDirectory("round-trip"));
    Scene& scene = fixture.scene;
    const SceneDocumentOptions& options = fixture.document.options();
    PrefabAuthoring authoring(fixture.assets, options);
    // Module and preserved unknown components, and a child.
    const Entity lamp = authorLamp(scene);
    scene.registry().get<TransformComponent>(lamp).local[3] = glm::vec4(3.0f, 0.0f, 0.0f, 1.0f);
    expect(authoring.create(scene, lamp, "lamp.prefab").success, "the round-trip lamp should be created");
    // A nested prefab and entity references into and out of it.
    const Entity wheel = scene.createEntity("Wheel");
    const Entity hub = scene.createEntity("Hub");
    scene.setParent(hub, wheel);
    expect(authoring.create(scene, wheel, "parts/wheel.prefab").success, "the round-trip wheel should be created");
    const Entity cart = scene.createEntity("Cart");
    const Entity axle = scene.createEntity("Axle");
    scene.setParent(axle, cart);
    scene.setParent(wheel, cart);
    scene.registry().get<TransformComponent>(wheel).local[3] = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    scene.registry().emplace<FollowComponent>(axle, FollowComponent{.target = scene.id(hub)});
    scene.registry().emplace<FollowComponent>(
        hub, FollowComponent{.target = scene.id(axle), .settings = {.target = scene.id(axle), .distance = 3.0f}});
    expect(authoring.create(scene, cart, "cart.prefab").success, "the round-trip cart should be created");

    for (const char* reference : {"lamp.prefab", "parts/wheel.prefab", "cart.prefab"}) {
        expect(reopenKeepsBytes(fixture.root, fixture.assets, options, reference),
               "opening and saving an editor-written prefab without edits should keep every byte");
    }

    // The engine capture alone, as an editor saving a prefab document calls it.
    PrefabSystem prefabs(fixture.assets, options);
    Scene empty;
    const PrefabInstanceResult cartInstance = prefabs.instantiate("cart.prefab", empty);
    PrefabCapture capture;
    std::string serialized;
    expect(cartInstance.success &&
               prefabs
                   .capture(empty, cartInstance.root, "cart.prefab", capture,
                            PrefabCaptureOptions{.keepRootTransform = true})
                   .success &&
               serializePrefab(capture.document, serialized).success &&
               serialized + '\n' == readText(fixture.root / "cart.prefab"),
           "capturing an instantiated prefab with keepRootTransform should reproduce its file");

    // An edited root transform is the prefab's content there.
    Scene document;
    OpenedPrefabDocument opened;
    expect(openPrefabDocument("lamp.prefab", document, prefabs, opened).success && opened.displayName == "Lamp",
           "the lamp should open with its display name");
    const Entity openedRoot = document.findEntity(opened.root);
    expect(openedRoot != entt::null && document.registry().get<TransformComponent>(openedRoot).local[3].x == 3.0f,
           "the opened root should carry the prefab's root transform");
    document.registry().get<TransformComponent>(openedRoot).local[3] = glm::vec4(0.0f, 5.0f, 0.0f, 1.0f);
    const Entity extra = document.createEntity("Extra");
    document.setParent(extra, openedRoot);
    SavedPrefabDocument saved;
    expect(savePrefabDocument(document, opened.root, "lamp.prefab", prefabs, saved).success &&
               saved.before == opened.fileBytes && saved.after != opened.fileBytes,
           "saving an edited prefab document should report both contents");
    PrefabDocument reloaded;
    expect(loadPrefab(fixture.root / "lamp.prefab", reloaded).success && reloaded.entities.size() == 3 &&
               reloaded.entities.front().localTransform[3].y == 5.0f,
           "an edited root transform and an added entity should be saved");
    expect(document.registry().all_of<PrefabEntityComponent>(extra),
           "saving should link an added entity, so the next save keeps its id");
    expect(reopenKeepsBytes(fixture.root, fixture.assets, options, "lamp.prefab"),
           "a saved prefab document should reopen and save unchanged");

    // An apply still keeps the prefab's own root placement.
    Entity instance = entt::null;
    expect(authoring.instantiate(scene, "lamp.prefab", entt::null, instance).success, "the lamp should instantiate");
    scene.registry().get<TransformComponent>(instance).local[3] = glm::vec4(9.0f, 0.0f, 0.0f, 1.0f);
    expect(authoring.apply(scene, instance).success && loadPrefab(fixture.root / "lamp.prefab", reloaded).success &&
               reloaded.entities.front().localTransform[3].y == 5.0f &&
               reloaded.entities.front().localTransform[3].x == 0.0f,
           "apply should keep the prefab's root transform");

    Scene occupied;
    occupied.createEntity("Existing");
    expect(!openPrefabDocument("lamp.prefab", occupied, prefabs, opened),
           "a prefab should open only into an empty scene");
    Scene missing;
    expect(!openPrefabDocument("missing.prefab", missing, prefabs, opened) &&
               missing.registry().view<const SceneEntityIdComponent>().empty(),
           "opening a missing prefab should fail without changing the scene");
}
} // namespace

int main() {
    testRegistryRoundTrip();
    testVersion1Migration();
    testLinkPersistence();
    testOverrideQuery();
    testOperators();
    testAddCharacter();
    testNestedPrefabs();
    testFailedRefreshRestoresScene();
    testApplyPropagation();
    testCollectionsStayInScene();
    testEntityReferenceRemap();
    testStructuralOverrides();
    testNestedInstanceRemoval();
    testMovesOutOfAndWithinInstances();
    testMovesUnderAddedWithoutCycles();
    testNestedReferences();
    testOpenAndSaveRoundTrip();
    for (const char* name : {"registry", "v1", "persistence", "overrides", "operators", "character", "nested",
                             "rollback", "propagation", "collections", "references", "structure", "nested-removal",
                             "moves", "move-cycles", "nested-references", "round-trip"}) {
        std::error_code error;
        std::filesystem::remove_all(
            std::filesystem::temp_directory_path() / ("zenith-prefab-tests-" + std::string(name)), error);
    }
    return failures == 0 ? 0 : 1;
}
