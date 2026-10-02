#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

enum class Mood : uint8_t {
    Calm,
    Angry,
};

struct TestSettings {
    float weight{1.0f};
    bool active{true};
};

// Stands in for a game-defined component: every reflected field kind.
struct TestBehaviourComponent {
    float speed{2.0f};
    int32_t lives{3};
    uint32_t score{0};
    glm::vec3 tint{1.0f};
    glm::vec4 glow{1.0f};
    glm::uvec3 cells{1u, 2u, 3u};
    std::string label{"hero"};
    Mood mood{Mood::Calm};
    float minimumDelay{0.5f};
    float maximumDelay{1.5f};
    TestSettings settings;
};

Fields<TestBehaviourComponent> behaviourFields() {
    using C = TestBehaviourComponent;
    return std::move(
        Fields<C>()
            .add("speed", &C::speed)
            .add("lives", &C::lives)
            .add("score", &C::score)
            .optional()
            .add("tint", &C::tint)
            .color()
            .add("glow", &C::glow)
            .add("cells", &C::cells)
            .add("label", &C::label)
            .add("mood", &C::mood)
            .options({{"calm", "Calm", 0}, {"angry", "Angry", 1}})
            .range("delay", &C::minimumDelay, &C::maximumDelay)
            .structure(
                "settings", &C::settings,
                Fields<TestSettings>().add("weight", &TestSettings::weight).add("active", &TestSettings::active)));
}

// Schema version 2 renamed "old_speed" to "speed".
ComponentType behaviourType() {
    return ComponentTypeBuilder<TestBehaviourComponent>("test_behaviour", "Test Behaviour")
        .category("Gameplay")
        .fields(behaviourFields())
        .schemaVersion(2)
        .migration(1,
                   [](nlohmann::json& component) {
                       if (const auto old = component.find("old_speed"); old != component.end()) {
                           component["speed"] = *old;
                           component.erase(old);
                       }
                       return SceneDocumentResult{.success = true};
                   })
        .validate([](const TestBehaviourComponent& behaviour) {
            return behaviour.lives < 0 ? std::string("Lives cannot be negative") : std::string{};
        })
        .build();
}

std::shared_ptr<const ComponentTypeRegistry> gameRegistry() {
    auto registry = std::make_shared<ComponentTypeRegistry>();
    expect(registerBuiltInComponentTypes(*registry).success, "built-in types should register into a game registry");
    const SceneDocumentResult added = registry->add(behaviourType());
    expect(added.success, added.error);
    return registry;
}

std::string documentWith(std::string_view components) {
    return R"({"version":1,"entities":[{"id":7,"name":"a","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],)"
           R"("parent":null,"components":)" +
           std::string(components) + "}]}";
}

std::string serialize(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string output;
    const SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, output);
    expect(result.success, result.error);
    return output;
}

void testBuiltIns() {
    const std::shared_ptr<const ComponentTypeRegistry> builtIns = builtInComponentTypes();
    expect(builtIns == builtInComponentTypes(), "the built-in registry should be created once");
    expect(builtIns->types().size() == 18, "every serialized engine component should be registered");
    const ComponentType* light = builtIns->find("point_light");
    expect(light != nullptr && light->displayName() == "Point Light" && !light->internal(),
           "point lights should be a user-facing type");
    expect(builtIns->find("model") != nullptr && builtIns->find("model")->internal(),
           "model instances should be internal");

    // Type-erased editing round trip used by the inspector.
    Scene scene;
    const Entity entity = scene.createEntity("Spot");
    const ComponentType* spot = builtIns->find("spot_light");
    spot->addDefault(scene, entity);
    auto* component = static_cast<SpotLightComponent*>(spot->find(scene, entity));
    expect(component != nullptr && spot->has(scene, entity), "a default component should be addable");
    const std::any original = spot->copy(component);
    component->innerConeRadians = 1.0f;
    component->outerConeRadians = 0.5f;
    expect(!spot->validate(component).empty(), "an outer cone narrower than the inner cone should be invalid");
    spot->sanitize(component);
    expect(component->outerConeRadians == 1.0f && spot->validate(component).empty(),
           "sanitizing should widen the outer cone to the inner cone");
    spot->assign(component, original);
    expect(component->innerConeRadians == SpotLightComponent{}.innerConeRadians,
           "assigning a copied value should restore it");
    spot->remove(scene, entity);
    expect(!spot->has(scene, entity), "a component should be removable");
}

void testGameComponentRoundTrip() {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = std::filesystem::temp_directory_path(),
                                       .componentTypes = gameRegistry()};
    Scene scene;
    const Entity entity = scene.createEntity("Hero");
    TestBehaviourComponent behaviour;
    behaviour.speed = 4.5f;
    behaviour.lives = 5;
    behaviour.score = 900;
    behaviour.tint = {0.1f, 0.2f, 0.3f};
    behaviour.cells = {4u, 5u, 6u};
    behaviour.label = "villain";
    behaviour.mood = Mood::Angry;
    behaviour.minimumDelay = 0.25f;
    behaviour.maximumDelay = 0.75f;
    behaviour.settings = {.weight = 3.0f, .active = false};
    scene.registry().emplace<TestBehaviourComponent>(entity, behaviour);

    const std::string document = serialize(scene, assets, options);
    const nlohmann::json parsed = nlohmann::json::parse(document);
    const nlohmann::json& saved = parsed.at("entities").at(0).at("components").at("test_behaviour");
    expect(saved.at("$version") == 2 && saved.at("mood") == "angry" && saved.at("delay").size() == 2 &&
               saved.at("settings").at("active") == false,
           "a reflected component should save its schema version, enum names, ranges, and nested structs");

    Scene restored;
    const SceneDocumentResult loaded = SceneDocument::deserialize(document, assets, options, restored);
    expect(loaded.success, loaded.error);
    const auto* restoredBehaviour =
        restored.registry().try_get<TestBehaviourComponent>(restored.findEntity(scene.id(entity)));
    expect(restoredBehaviour != nullptr && restoredBehaviour->speed == 4.5f && restoredBehaviour->score == 900 &&
               restoredBehaviour->cells == glm::uvec3(4u, 5u, 6u) && restoredBehaviour->label == "villain" &&
               restoredBehaviour->mood == Mood::Angry && restoredBehaviour->maximumDelay == 0.75f &&
               restoredBehaviour->settings.weight == 3.0f && !restoredBehaviour->settings.active,
           "a reflected component should round trip every field");
    expect(serialize(restored, assets, options) == document, "a reloaded game component should save identically");
}

void testMigrationsAndRejections() {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = std::filesystem::temp_directory_path(),
                                       .componentTypes = gameRegistry()};
    constexpr std::string_view versionOne =
        R"({"test_behaviour":{"old_speed":7,"lives":1,"tint":[1,1,1],"glow":[1,1,1,1],"cells":[1,1,1],)"
        R"("label":"x","mood":"calm","delay":[0,1],"settings":{"weight":1,"active":true}}})";
    Scene migrated;
    const SceneDocumentResult result = SceneDocument::deserialize(documentWith(versionOne), assets, options, migrated);
    expect(result.success, result.error);
    const auto* behaviour = migrated.registry().try_get<TestBehaviourComponent>(migrated.findEntity({7}));
    expect(behaviour != nullptr && behaviour->speed == 7.0f && behaviour->score == 0,
           "a version 1 component should migrate and keep optional defaults");

    const auto expectRejected = [&](std::string_view components, std::string_view expected) {
        Scene scene;
        const SceneDocumentResult rejected =
            SceneDocument::deserialize(documentWith(components), assets, options, scene);
        expect(!rejected.success && rejected.error.find(expected) != std::string::npos,
               std::string("expected rejection mentioning '") + std::string(expected) + "', got: " + rejected.error);
    };
    expectRejected(R"({"test_behaviour":{"$version":3}})", "newer than the supported version");
    expectRejected(R"({"test_behaviour":{"$version":2,"speed":1}})", "Missing required field 'lives'");
    expectRejected(
        R"({"test_behaviour":{"$version":2,"speed":"fast","lives":1,"tint":[1,1,1],"glow":[1,1,1,1],"cells":[1,1,1],)"
        R"("label":"x","mood":"calm","delay":[0,1],"settings":{"weight":1,"active":true}}})",
        "Field 'speed' must be a number");
    expectRejected(
        R"({"test_behaviour":{"$version":2,"speed":1,"lives":-2,"tint":[1,1,1],"glow":[1,1,1,1],"cells":[1,1,1],)"
        R"("label":"x","mood":"sulky","delay":[0,1],"settings":{"weight":1,"active":true}}})",
        "Field 'mood' must be one of calm angry");
    expectRejected(
        R"({"test_behaviour":{"$version":2,"speed":1,"lives":-2,"tint":[1,1,1],"glow":[1,1,1,1],"cells":[1,1,1],)"
        R"("label":"x","mood":"calm","delay":[0,1],"settings":{"weight":1,"active":true}}})",
        "Lives cannot be negative");
}

void testUnknownComponentsArePreserved() {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = std::filesystem::temp_directory_path()};
    constexpr std::string_view components = R"({"camera":{"vertical_fov":1,"near_plane":0.1,"far_plane":100,)"
                                            R"("primary":true},"game_inventory":{"slots":[1,2,3],"owner":"hero"}})";
    Scene scene;
    const SceneDocumentResult loaded = SceneDocument::deserialize(documentWith(components), assets, options, scene);
    expect(loaded.success, loaded.error);
    const auto* preserved = scene.registry().try_get<PreservedComponentsComponent>(scene.findEntity({7}));
    expect(preserved != nullptr && preserved->components.size() == 1 &&
               preserved->components.front().first == "game_inventory",
           "a component the registry does not know should be preserved");
    const nlohmann::json saved = nlohmann::json::parse(serialize(scene, assets, options));
    expect(saved.at("entities").at(0).at("components").at("game_inventory") ==
               nlohmann::json::parse(R"({"slots":[1,2,3],"owner":"hero"})"),
           "a preserved component should be saved unchanged");

    // A registry that knows the key reads it instead of preserving it.
    const SceneDocumentOptions gameOptions{.assetRoot = options.assetRoot, .componentTypes = gameRegistry()};
    Scene known;
    expect(SceneDocument::deserialize(documentWith(R"({"test_behaviour":{"$version":3}})"), assets, options, known)
               .success,
           "documents should load with types the registry does not know");
    expect(!SceneDocument::deserialize(documentWith(R"({"test_behaviour":{"$version":3}})"), assets, gameOptions, known)
                .success,
           "a known type should be read rather than preserved");
}

void testRegistryRejectsInvalidTypes() {
    ComponentTypeRegistry registry;
    expect(registry.add(behaviourType()).success, "a valid type should register");
    expect(!registry.add(behaviourType()).success, "a duplicate key should be rejected");

    const SceneDocumentResult missingMigration =
        registry.add(ComponentTypeBuilder<TestSettings>("settings_v3", "Settings").schemaVersion(3).build());
    expect(!missingMigration.success && missingMigration.error.find("migration") != std::string::npos,
           "a schema version without migrations from every earlier version should be rejected");

    const SceneDocumentResult enumWithoutOptions =
        registry.add(ComponentTypeBuilder<TestBehaviourComponent>("no_options", "No Options")
                         .fields(Fields<TestBehaviourComponent>().add("mood", &TestBehaviourComponent::mood))
                         .build());
    expect(!enumWithoutOptions.success && enumWithoutOptions.error.find("options") != std::string::npos,
           "an enum field without options should be rejected");

    const SceneDocumentResult unreflectedTexture = registry.add(
        ComponentTypeBuilder<EnvironmentLightComponent>("bad_texture", "Bad Texture")
            .texture(&EnvironmentLightComponent::environmentAsset, &EnvironmentLightComponent::environmentMap)
            .build());
    expect(!unreflectedTexture.success, "a texture path that is not a reflected field should be rejected");
    expect(!registry.add(ComponentTypeBuilder<TestSettings>("$reserved", "Reserved").build()).success,
           "keys starting with '$' are reserved for document metadata");
}
} // namespace

int main() {
    expect(labelFromKey("shadow_resolution") == "Shadow resolution", "labels should be derived from keys");
    testBuiltIns();
    testGameComponentRoundTrip();
    testMigrationsAndRejections();
    testUnknownComponentsArePreserved();
    testRegistryRejectsInvalidTypes();
    if (failures != 0) {
        std::cerr << failures << " component type test(s) failed\n";
        return 1;
    }
    return 0;
}
