// The demo gameplay as its hosts see it: the zenith_game_demo module the editor
// and the game load for the demo project, and the same registration linked
// into the game for runs without a module.

#include "engine/application/Application.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/module/GameModule.hpp"
#include "engine/module/SystemScheduler.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "game/application/DemoApplication.hpp"
#include "game/application/LinkedGameplay.hpp"
#include "game/application/RuntimeGameplay.hpp"
#include "game/gameplay/DemoGameplay.hpp"
#include "game/gameplay/ItemCollection.hpp"
#include "game/player/FirstPersonController.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
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

const FieldDescriptor* findField(const ComponentType& type, std::string_view key) {
    for (const FieldDescriptor& field : type.fields()) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}

float floatField(const ComponentType& type, const void* component, std::string_view key) {
    const FieldDescriptor* field = findField(type, key);
    return field != nullptr ? field->value<float>(component) : -1.0f;
}

void setFloatField(const ComponentType& type, void* component, std::string_view key, float value) {
    if (const FieldDescriptor* field = findField(type, key); field != nullptr) {
        field->value<float>(component) = value;
    }
}

const std::vector<std::string> viewSystems{std::string(DemoGameplay::FirstPersonViewSystem)};
const std::vector<std::string> fixedSystems{std::string(DemoGameplay::FirstPersonMovementSystem),
                                            std::string(DemoGameplay::ItemCollectionSystem)};

void expectGameplaySchedule(const SystemScheduler& scheduler, const char* host) {
    const bool playScheduled = scheduler.schedule(SystemPhase::Update, SimulationMode::Play) == viewSystems &&
                               scheduler.schedule(SystemPhase::FixedUpdate, SimulationMode::Play) == fixedSystems;
    const bool editIdle = scheduler.schedule(SystemPhase::Update, SimulationMode::Edit).empty() &&
                          scheduler.schedule(SystemPhase::FixedUpdate, SimulationMode::Edit).empty();
    if (!playScheduled || !editIdle) {
        std::cerr << "In the " << host << ":\n";
    }
    expect(playScheduled, "view, movement, and collection systems run in play mode and registration order");
    expect(editIdle, "demo gameplay systems do not run while a scene is edited");
}

void expectControllerType(const ComponentType* type) {
    expect(type != nullptr && type->displayName() == "Character Controller" && type->category() == "Gameplay" &&
               !type->internal(),
           "character_controller is an authorable gameplay component");
    if (type == nullptr) {
        return;
    }
    const FieldDescriptor* yaw = findField(*type, "yaw");
    const FieldDescriptor* airControl = findField(*type, "air_control");
    expect(findField(*type, "radius") != nullptr && findField(*type, "move_speed") != nullptr &&
               findField(*type, "look_sensitivity") != nullptr && yaw != nullptr && airControl != nullptr,
           "the controller's tuning fields are reflected");
    expect(yaw != nullptr && yaw->presentation.angle && airControl != nullptr &&
               airControl->presentation.widget == NumberWidget::Slider,
           "controller fields carry inspector hints");
    expect(findField(*type, "grounded") == nullptr && findField(*type, "camera") == nullptr,
           "runtime controller state is not serialized");
}

void testModule(const std::filesystem::path& modulePath, const std::filesystem::path& shadowDirectory) {
    std::unique_ptr<GameModule> module;
    const GameModuleResult loaded = GameModule::load(modulePath, {.shadowDirectory = shadowDirectory}, module);
    expect(loaded.success && module != nullptr, "the zenith_game_demo module loads");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }
    expect(module->name() == "demo", "the module identifies itself as the demo");
    const std::span<const std::string> keys = module->moduleComponentKeys();
    expect(keys.size() == 3 && keys[0] == DemoGameplay::CharacterControllerKey &&
               keys[1] == DemoGameplay::CollectibleItemKey && keys[2] == DemoGameplay::ItemCollectorKey,
           "the module registers the controller and item collection components");

    {
        std::shared_ptr<const ComponentTypeRegistry> types = module->componentTypes();
        const ComponentType* controllerType = types->find(DemoGameplay::CharacterControllerKey);
        const ComponentType* itemType = types->find(DemoGameplay::CollectibleItemKey);
        const ComponentType* collectorType = types->find(DemoGameplay::ItemCollectorKey);
        expectControllerType(controllerType);
        expect(itemType != nullptr && findField(*itemType, "value") != nullptr,
               "collectible items expose their positive value");
        expect(collectorType != nullptr && collectorType->fields().empty(),
               "item collectors expose no authored runtime counter");
        const SystemScheduler scheduler(module->systems());
        expectGameplaySchedule(scheduler, "module");
        if (controllerType == nullptr || collectorType == nullptr) {
            return;
        }

        // Scenes touching module components are created and destroyed while
        // the module is loaded, through the module's own type functions.
        AssetManager assets;
        const SceneDocumentOptions documentOptions{.componentTypes = types};
        std::string serialized;
        {
            Scene scene;
            const Entity player = scene.createEntity("Player");
            controllerType->addDefault(scene, player);
            collectorType->addDefault(scene, player);
            void* controller = controllerType->find(scene, player);
            expect(floatField(*controllerType, controller, "move_speed") == 4.0f,
                   "a controller added in the editor starts with the game's defaults");
            setFloatField(*controllerType, controller, "move_speed", 6.5f);
            expect(SceneDocument::serialize(scene, assets, documentOptions, serialized).success &&
                       serialized.find(DemoGameplay::CharacterControllerKey) != std::string::npos &&
                       serialized.find("\"item_collector\": {}") != std::string::npos,
                   "scenes serialize controllers and the collector tag without runtime state");

            setFloatField(*controllerType, controller, "radius", 0.0f);
            expect(!controllerType->validate(controller).empty(), "a controller without a radius is invalid");
        }

        Scene reloaded;
        expect(SceneDocument::deserialize(serialized, assets, documentOptions, reloaded).success,
               "scenes with character controllers load through the module's registry");
        Entity reloadedPlayer = entt::null;
        for (const Entity candidate : reloaded.registry().view<NameComponent>()) {
            if (controllerType->find(reloaded, candidate) != nullptr) {
                reloadedPlayer = candidate;
            }
        }
        expect(reloadedPlayer != entt::null &&
                   floatField(*controllerType, controllerType->find(reloaded, reloadedPlayer), "move_speed") == 6.5f,
               "character controller values survive a document round trip");
        if (reloadedPlayer == entt::null) {
            return;
        }

        scheduler.run(SystemPhase::Update, {.scene = reloaded, .mode = SimulationMode::Play, .deltaSeconds = 0.1f});
        expect(floatField(*controllerType, controllerType->find(reloaded, reloadedPlayer), "yaw") == 0.0f,
               "the view system ignores frames without input");
        InputState look;
        look.mouseDelta = {100.0f, 0.0f};
        scheduler.run(SystemPhase::Update,
                      {.scene = reloaded, .mode = SimulationMode::Play, .deltaSeconds = 0.1f, .input = &look});
        expect(floatField(*controllerType, controllerType->find(reloaded, reloadedPlayer), "yaw") < 0.0f,
               "the module's view system turns look input into yaw");
        // Without application services there is no physics to move; the
        // movement system must leave the scene alone rather than fail.
        scheduler.run(SystemPhase::FixedUpdate,
                      {.scene = reloaded, .mode = SimulationMode::Play, .deltaSeconds = 0.1f, .input = &look});
        expect(floatField(*controllerType, controllerType->find(reloaded, reloadedPlayer), "move_speed") == 6.5f,
               "the movement system tolerates a host without application services");
    }
    module.reset();
}

void testLinkedGameplay() {
    Gameplay gameplay;
    const GameplayResult linked = linkDemoGameplay(gameplay);
    expect(linked.success && gameplay.componentTypes != nullptr && gameplay.systems != nullptr,
           "the game links the demo gameplay through a module registrar");
    if (!linked) {
        std::cerr << linked.error << '\n';
        return;
    }
    expect(gameplay.componentTypes->types().size() == builtInComponentTypes()->types().size() + 3 &&
               gameplay.componentTypes->find(DemoGameplay::CollectibleItemKey) != nullptr &&
               gameplay.componentTypes->find(DemoGameplay::ItemCollectorKey) != nullptr &&
               gameplay.componentTypes->find("point_light") != nullptr,
           "linked gameplay extends the built-in component types");
    expectControllerType(gameplay.componentTypes->find(DemoGameplay::CharacterControllerKey));
    expectGameplaySchedule(*gameplay.systems, "game");
}

void testItemCollectionBehavior() {
    Scene scene;
    const Entity collector = scene.createEntity("Collector");
    scene.registry().emplace<ItemCollectorComponent>(collector);
    const Entity firstItem = scene.createEntity("First item");
    scene.registry().emplace<CollectibleItemComponent>(firstItem, CollectibleItemComponent{.value = 2});
    const SceneEntityId firstItemId = scene.id(firstItem);
    const Entity child = scene.createEntity("Item child");
    scene.setParent(child, firstItem);
    const Entity secondItem = scene.createEntity("Second item");
    scene.registry().emplace<CollectibleItemComponent>(secondItem, CollectibleItemComponent{.value = 3});
    const SceneEntityId secondItemId = scene.id(secondItem);
    const Entity unrelated = scene.createEntity("Unrelated");

    expect(itemCollectionProgress(scene).collected == 0 && itemCollectionProgress(scene).total == 5,
           "collection progress includes all authored item values");
    const std::vector<ContactEvent> firstContacts{
        {.type = ContactEventType::Begin, .first = firstItem, .second = collector},
        {.type = ContactEventType::Begin, .first = collector, .second = firstItem},
        {.type = ContactEventType::Persist, .first = collector, .second = secondItem},
        {.type = ContactEventType::Begin, .first = collector, .second = unrelated},
    };
    ItemCollectionSystem{}.fixedUpdate(scene, firstContacts);
    expect(scene.findEntity(firstItemId) == entt::null && !scene.registry().valid(child),
           "a collected item and its children are removed");
    expect(scene.registry().get<ItemCollectorComponent>(collector).collectedValue == 2,
           "duplicate contacts collect an item exactly once");
    expect(itemCollectionProgress(scene).collected == 2 && itemCollectionProgress(scene).total == 5,
           "collection progress remains stable as items disappear");

    ItemCollectionSystem{}.fixedUpdate(
        scene, std::array{ContactEvent{.type = ContactEventType::Begin, .first = collector, .second = secondItem}});
    expect(scene.registry().get<ItemCollectorComponent>(collector).collectedValue == 5 &&
               scene.findEntity(secondItemId) == entt::null && itemCollectionProgress(scene).total == 5,
           "later begin contacts collect remaining item values");
}

void testPhysicsSensorCollection() {
    PhysicsWorld physics;
    PhysicsWorldConfig config;
    config.gravity = glm::vec3(0.0f);
    expect(static_cast<bool>(physics.init(config)), "collection physics world initializes");
    AssetManager assets;
    Scene scene;
    const Entity collector = scene.createEntity("Collector");
    scene.registry().emplace<ItemCollectorComponent>(collector);
    scene.registry().emplace<ColliderComponent>(collector, SphereCollider{0.5f});
    RigidBodyComponent collectorBody;
    collectorBody.motionType = PhysicsMotionType::Dynamic;
    collectorBody.layer = PhysicsLayer::Player;
    collectorBody.gravityFactor = 0.0f;
    scene.registry().emplace<RigidBodyComponent>(collector, collectorBody);

    const Entity item = scene.createEntity("Sensor item");
    const SceneEntityId itemId = scene.id(item);
    scene.registry().emplace<CollectibleItemComponent>(item);
    scene.registry().emplace<ColliderComponent>(item, SphereCollider{0.5f});
    RigidBodyComponent itemBody;
    itemBody.sensor = true;
    scene.registry().emplace<RigidBodyComponent>(item, itemBody);

    expect(static_cast<bool>(physics.step(scene, assets, config.fixedTimeStep)), "sensor physics step succeeds");
    expect(!physics.contactEvents().empty(), "overlapping collector and sensor produce a contact");
    ItemCollectionSystem{}.fixedUpdate(scene, physics.contactEvents());
    expect(scene.registry().get<ItemCollectorComponent>(collector).collectedValue == 1 &&
               scene.findEntity(itemId) == entt::null,
           "real sensor contacts collect and remove an item");
    physics.shutdown();
}

void testCheckedInCollectionLevel() {
    Gameplay gameplay;
    expect(static_cast<bool>(linkDemoGameplay(gameplay)), "checked-in level gameplay links");
    ProjectConfig project;
    const ProjectConfigResult projectResult =
        loadProjectConfig(std::filesystem::path(ZENITH_PROJECT_DIR) / "demo.zenith-project", project);
    expect(static_cast<bool>(projectResult), "checked-in demo project loads");
    if (!projectResult) {
        std::cerr << projectResult.error << '\n';
        return;
    }
    AssetManager assets;
    Scene scene;
    const ProjectConfigResult sceneResult = loadProjectStartupScene(project, assets, scene, gameplay.componentTypes);
    expect(static_cast<bool>(sceneResult), "checked-in collection level loads with demo component types");
    if (!sceneResult) {
        std::cerr << sceneResult.error << '\n';
        return;
    }

    const auto players = scene.registry().view<CharacterControllerComponent, ItemCollectorComponent>();
    expect(std::distance(players.begin(), players.end()) == 1, "collection level has one authored playable collector");
    expect(scene.registry().view<CollectibleItemComponent>().size() == 5, "collection level has five authored pickups");
    expect(scene.registry().view<CameraComponent>().size() == 1, "collection level has one authored camera");
    size_t solidBodies = 0;
    size_t sensorBodies = 0;
    for (const Entity entity : scene.registry().view<ColliderComponent, RigidBodyComponent>()) {
        if (scene.registry().get<RigidBodyComponent>(entity).sensor) {
            ++sensorBodies;
        } else {
            ++solidBodies;
        }
    }
    expect(solidBodies == 9 && sensorBodies == 5,
           "collection level has eight walkable map colliders, one player body, and five sensors");
}

// A controller authored in a project scene is the game's player: no second
// player spawns, its camera child is adopted, and its tuning survives.
void testAuthoredControllerPlays(const std::filesystem::path& directory) {
    Gameplay gameplay;
    if (!linkDemoGameplay(gameplay)) {
        expect(false, "the game links the demo gameplay for the authored scene");
        return;
    }
    ProjectConfig project;
    project.projectFile = directory / "authored.zenith-project";
    project.displayName = "Authored";
    project.assetRoot = directory / "assets";
    project.sceneRoot = directory / "scenes";
    project.startupScene = project.sceneRoot / "authored.zenith-scene";
    std::error_code error;
    std::filesystem::create_directories(project.sceneRoot, error);
    {
        AssetManager assets;
        Scene authored;
        const Entity hero = authored.createEntity("Hero");
        authored.registry().emplace<CharacterControllerComponent>(hero,
                                                                  CharacterControllerComponent{.moveSpeed = 7.0f});
        const Entity camera = authored.createEntity("Hero Camera");
        authored.registry().emplace<CameraComponent>(camera);
        authored.setParent(camera, hero);
        expect(SceneDocument::save(project.startupScene, authored, assets,
                                   {.assetRoot = project.assetRoot, .componentTypes = gameplay.componentTypes})
                   .success,
               "a scene with an authored controller saves");
    }

    DemoApplication application(project);
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    ApplicationSession session(application, scene, assets, physics, audio);
    const SceneResult started = session.start();
    expect(started.success, "the demo starts a project scene holding a character controller");
    if (!started) {
        std::cerr << started.error << '\n';
        return;
    }
    const auto controllers = scene.registry().view<CharacterControllerComponent, NameComponent>();
    expect(std::distance(controllers.begin(), controllers.end()) == 1,
           "an authored controller replaces the spawned player");
    if (controllers.begin() == controllers.end()) {
        session.shutdown();
        return;
    }
    const Entity hero = *controllers.begin();
    expect(controllers.get<NameComponent>(hero).name == "Hero" &&
               controllers.get<CharacterControllerComponent>(hero).moveSpeed == 7.0f,
           "the authored controller keeps its name and tuning");
    expect(scene.registry().all_of<ColliderComponent, RigidBodyComponent>(hero) &&
               scene.registry().get<RigidBodyComponent>(hero).lockRotation,
           "an authored controller without a body gets the player's collider and body");
    expect(scene.registry().view<AudioListenerComponent>().size() == 1,
           "an authored player's camera becomes the audio listener");

    InputState startGame;
    startGame.setKeyPressed(Key::Space);
    session.update(startGame, 1.0f / 60.0f);
    session.drawUi();
    InputState look;
    look.mouseDelta = {100.0f, 20.0f};
    session.update(look, 1.0f / 60.0f);
    const auto& controller = scene.registry().get<CharacterControllerComponent>(hero);
    expect(controller.yawRadians < 0.0f && controller.pitchRadians < 0.0f,
           "the game's linked view system turns the authored controller");
    expect(controller.camera != entt::null &&
               scene.registry().get<NameComponent>(controller.camera).name == "Hero Camera" &&
               scene.registry().get<TransformComponent>(controller.camera).local != glm::mat4(1.0f),
           "the authored camera child is adopted and oriented");
    session.shutdown();
}

// The checked-in demo project plays through the zenith_game_demo module, as
// the game runs it: the demo presents it, and its authored player turns with
// the module's view system.
void testDemoProjectPlaysThroughModule() {
    ProjectConfig project;
    if (!loadProjectConfig(std::filesystem::path(ZENITH_PROJECT_DIR) / "demo.zenith-project", project)) {
        expect(false, "checked-in demo project loads for the module run");
        return;
    }
    expect(project.gameModule.filename() == "zenith_game_demo.zmodule", "the demo project names the demo module");
    // This build's module, whichever configuration last refreshed the copy
    // beside the project.
    project.gameModule = ZENITH_DEMO_GAME_MODULE;
    // Before every scene: the module's code must outlive them. In place, as
    // the shared default shadow folder is cleaned by concurrent tests.
    RuntimeGameplay runtime;
    const GameplayResult loaded = loadRuntimeGameplay(&project, GameModuleLoading::InPlace, runtime);
    expect(loaded.success && runtime.module != nullptr && runtime.presentation == GamePresentation::Demo,
           "the demo project's module loads and the demo presents it");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }

    DemoApplication application(project, {}, std::nullopt, runtime.gameplay);
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    ApplicationSession session(application, scene, assets, physics, audio);
    const SceneResult started = session.start();
    expect(started.success, "the demo project starts through its module");
    if (!started) {
        std::cerr << started.error << '\n';
        return;
    }
    const auto controllers = scene.registry().view<CharacterControllerComponent>();
    expect(controllers.size() == 1 && scene.registry().view<CollectibleItemComponent>().size() == 5,
           "the module's component types load the authored player and pickups");
    if (controllers.empty()) {
        session.shutdown();
        return;
    }
    InputState startGame;
    startGame.setKeyPressed(Key::Space);
    session.update(startGame, 1.0f / 60.0f);
    session.drawUi();
    InputState look;
    look.mouseDelta = {100.0f, 0.0f};
    session.update(look, 1.0f / 60.0f);
    expect(scene.registry().get<CharacterControllerComponent>(controllers.front()).yawRadians < 0.0f,
           "the module's view system turns the authored player");
    session.shutdown();
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_demo_game_module_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    testModule(ZENITH_DEMO_GAME_MODULE, directory / "shadow");
    testLinkedGameplay();
    testItemCollectionBehavior();
    testPhysicsSensorCollection();
    testCheckedInCollectionLevel();
    testAuthoredControllerPlays(directory);
    testDemoProjectPlaysThroughModule();

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
