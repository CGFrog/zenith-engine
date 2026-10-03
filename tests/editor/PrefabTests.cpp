#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
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

std::shared_ptr<const ComponentTypeRegistry> moduleComponentTypes() {
    auto registry = std::make_shared<ComponentTypeRegistry>();
    expect(registerBuiltInComponentTypes(*registry).success, "built-in component types should register");
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
    fixture.editing().undo();
    expect(!scene.registry().all_of<PrefabInstanceComponent>(fixture.find(lampId)) &&
               std::filesystem::is_regular_file(prefabPath),
           "undoing create should unlink the entity and keep the written file");
    fixture.editing().redo();
    expect(scene.registry().all_of<PrefabInstanceComponent>(fixture.find(lampId)), "redo should restore the link");

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
    expect(fixture.invoke("prefab.apply").status == OperatorStatus::Finished,
           "prefab.apply should write the instance into its prefab");
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
    expect(scene.registry().get<PointLightComponent>(fixture.find(secondId)).intensity == 10.0f,
           "undoing apply should restore the other instances");
    fixture.editing().redo();
    expect(scene.registry().get<PointLightComponent>(fixture.find(secondId)).intensity == 77.0f,
           "redoing apply should update them again");

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
} // namespace

int main() {
    testRegistryRoundTrip();
    testVersion1Migration();
    testLinkPersistence();
    testOverrideQuery();
    testOperators();
    testNestedPrefabs();
    testApplyPropagation();
    for (const char* name : {"registry", "v1", "persistence", "overrides", "operators", "nested", "propagation"}) {
        std::error_code error;
        std::filesystem::remove_all(
            std::filesystem::temp_directory_path() / ("zenith-prefab-tests-" + std::string(name)), error);
    }
    return failures == 0 ? 0 : 1;
}
