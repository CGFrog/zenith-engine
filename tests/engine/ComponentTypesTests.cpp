#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
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
    // One reference to an entity in the scene and one to an entity that no
    // longer exists, which must be kept as it is.
    SceneEntityId target;
    SceneEntityId lostTarget;
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
            .structure("settings", &C::settings,
                       Fields<TestSettings>().add("weight", &TestSettings::weight).add("active", &TestSettings::active))
            .add("target", &C::target)
            .optional()
            .add("lost_target", &C::lostTarget)
            .optional());
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
    // 18 engine components, collection membership, collection instance, and
    // visibility, the two prefab link types, the two audio types, and the
    // material override.
    expect(builtIns->types().size() == 31, "every serialized engine component should be registered");
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

const FieldDescriptor* fieldOf(const ComponentType& type, std::string_view key) {
    const auto fields = type.fields();
    const auto found = std::ranges::find(fields, key, &FieldDescriptor::key);
    return found == fields.end() ? nullptr : &*found;
}

struct SkinnedSign {
    std::string picture;
    std::string backdrop;
    std::string notes;
    TextureHandle pictureTexture;
};

void testAssetKindHints() {
    const std::shared_ptr<const ComponentTypeRegistry> builtIns = builtInComponentTypes();
    const auto kindOf = [&](std::string_view type, std::string_view key) {
        const FieldDescriptor* field = fieldOf(*builtIns->find(type), key);
        return field == nullptr ? std::string("<missing>") : field->presentation.assetKind;
    };
    const auto editable = [&](std::string_view type, std::string_view key) {
        const FieldDescriptor* field = fieldOf(*builtIns->find(type), key);
        return field != nullptr && !field->presentation.readOnly;
    };
    expect(kindOf("decal", "albedo_asset") == "texture" && kindOf("decal", "normal_asset") == "texture" &&
               editable("decal", "albedo_asset") && editable("decal", "normal_asset"),
           "decal maps should be editable texture fields");
    expect(kindOf("particle_emitter", "texture_asset") == "texture" && editable("particle_emitter", "texture_asset"),
           "a particle sprite should be an editable texture field");
    expect(kindOf("environment_light", "environment_asset") == "environment" &&
               kindOf("reflection_probe", "environment_asset") == "environment" &&
               editable("environment_light", "environment_asset"),
           "environment maps should be editable environment fields");
    expect(kindOf("audio_emitter", "clip_asset") == "audio", "an audio clip should be an audio field");
    expect(kindOf("irradiance_volume", "bake_asset") == "bake" && !editable("irradiance_volume", "bake_asset"),
           "an irradiance bake should stay a read-only bake field");
    expect(kindOf("material_override", "material_asset") == "material" &&
               builtIns->find("material_override")->schemaVersion() == 2,
           "a material override should take a material file at schema version 2");

    // texture() and assetReference() fill in a kind the descriptor leaves
    // empty, and keep one it names.
    ComponentTypeRegistry registry;
    expect(registry
               .add(ComponentTypeBuilder<SkinnedSign>("skinned_sign", "Skinned Sign")
                        .fields(std::move(Fields<SkinnedSign>()
                                              .add("picture", &SkinnedSign::picture)
                                              .add("backdrop", &SkinnedSign::backdrop)
                                              .asset("environment")
                                              .add("notes", &SkinnedSign::notes)))
                        .texture(&SkinnedSign::picture, &SkinnedSign::pictureTexture)
                        .assetReference(&SkinnedSign::backdrop)
                        .assetReference(&SkinnedSign::notes)
                        .build())
               .success,
           "a type with asset fields should register");
    const ComponentType& sign = *registry.find("skinned_sign");
    expect(fieldOf(sign, "picture")->presentation.assetKind == "texture", "texture() should default to texture");
    expect(fieldOf(sign, "backdrop")->presentation.assetKind == "environment",
           "an explicit asset kind should win over the builder's default");
    expect(fieldOf(sign, "notes")->presentation.assetKind == "asset", "assetReference() should default to any asset");
    SkinnedSign value;
    expect(sign.assetLoaded(&value, "picture") == false && !sign.assetLoaded(&value, "notes"),
           "only loaded bindings should report a load state");
}

void testBuiltInRegistrationOrder() {
    // Registration order is the order documents are read in and the order the
    // editor lists types, so splitting the registration across files must not
    // change it.
    const std::vector<std::string_view> expected{"model",
                                                 "model_node",
                                                 "animation",
                                                 "morph_weights",
                                                 "skin",
                                                 "mesh_renderer",
                                                 "camera",
                                                 "directional_light",
                                                 "environment_light",
                                                 "volumetric_fog",
                                                 "particle_emitter",
                                                 "point_light",
                                                 "spot_light",
                                                 "reflection_probe",
                                                 "decal",
                                                 "irradiance_volume",
                                                 "collider",
                                                 "rigid_body",
                                                 "collection_membership",
                                                 "collection_instance",
                                                 "visibility",
                                                 "prefab_instance",
                                                 "prefab_entity",
                                                 "character_controller",
                                                 "first_person_camera",
                                                 "animator",
                                                 "nav_agent",
                                                 "tags",
                                                 "audio_listener",
                                                 "audio_emitter",
                                                 "material_override"};
    std::vector<std::string_view> actual;
    for (const ComponentType& type : builtInComponentTypes()->types()) {
        actual.push_back(type.key());
    }
    expect(actual == expected, "built-in component types should register in their established order");
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
    behaviour.target = scene.id(entity);
    behaviour.lostTarget = SceneEntityId{987654321};
    scene.registry().emplace<TestBehaviourComponent>(entity, behaviour);

    const std::string document = serialize(scene, assets, options);
    const nlohmann::json parsed = nlohmann::json::parse(document);
    const nlohmann::json& saved = parsed.at("entities").at(0).at("components").at("test_behaviour");
    expect(saved.at("$version") == 2 && saved.at("mood") == "angry" && saved.at("delay").size() == 2 &&
               saved.at("settings").at("active") == false && saved.at("target") == scene.id(entity).value &&
               saved.at("lost_target") == 987654321,
           "a reflected component should save its schema version, enum names, ranges, and nested structs");

    Scene restored;
    const SceneDocumentResult loaded = SceneDocument::deserialize(document, assets, options, restored);
    expect(loaded.success, loaded.error);
    const auto* restoredBehaviour =
        restored.registry().try_get<TestBehaviourComponent>(restored.findEntity(scene.id(entity)));
    expect(restoredBehaviour != nullptr && restoredBehaviour->speed == 4.5f && restoredBehaviour->score == 900 &&
               restoredBehaviour->cells == glm::uvec3(4u, 5u, 6u) && restoredBehaviour->label == "villain" &&
               restoredBehaviour->mood == Mood::Angry && restoredBehaviour->maximumDelay == 0.75f &&
               restoredBehaviour->settings.weight == 3.0f && !restoredBehaviour->settings.active &&
               restoredBehaviour->target == scene.id(entity) &&
               restoredBehaviour->lostTarget == SceneEntityId{987654321},
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
    testBuiltInRegistrationOrder();
    testAssetKindHints();
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
