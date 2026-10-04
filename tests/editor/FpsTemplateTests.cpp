// The FPS project template (templates/fps_game): its committed starter scene
// and impact prefab are valid documents with the expected content, and a
// project generated from it names its scene, input actions, and tags.
//
// The scene and prefab are written by this program, so they stay documents
// the engine's own serializers produce: run it with --write <template folder>
// after changing what they hold, then commit the result.

#include "editor/document/DefaultScene.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "engine/input/InputActionsJson.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/ParticleEmitter.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/scene/Tags.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr std::string_view sceneFile = "scenes/main.zenith-scene";
constexpr std::string_view impactPrefab = "prefabs/impact.prefab";
constexpr int targetCount = 5;

// A game module's component, kept as JSON until the module that defines it
// loads (as a scene saved with the module would hold it).
void addModuleComponent(Scene& scene, Entity entity, std::string key, const Json& value) {
    scene.registry().get_or_emplace<PreservedComponentsComponent>(entity).components.emplace_back(std::move(key),
                                                                                                  value.dump());
}

void setLocal(Scene& scene, Entity entity, const glm::mat4& local) {
    scene.registry().get_or_emplace<TransformComponent>(entity).local = local;
    scene.markTransformDirty(entity);
}

// A cube model with a matching static box collider.
Entity addBlock(Scene& scene, AssetManager& assets, const fs::path& assetRoot, std::string name,
                const glm::vec3& position, const glm::vec3& halfSize) {
    Entity block = entt::null;
    if (!addCube(scene, assets, assetRoot, block)) {
        expect(false, "writer: the cube model loads");
        return scene.createEntity(name);
    }
    scene.registry().get<NameComponent>(block).name = std::move(name);
    setLocal(scene, block, glm::scale(glm::translate(glm::mat4(1.0f), position), halfSize));
    scene.registry().emplace<ColliderComponent>(
        block, ColliderComponent{.shape = BoxCollider{.halfExtents = glm::vec3(1.0f)}});
    scene.registry().emplace<RigidBodyComponent>(block);
    return block;
}

// The course: a floor, walls, the player at the start, targets to shoot, and
// an exit sensor at the far end.
void buildScene(Scene& scene, AssetManager& assets, const fs::path& assetRoot) {
    addSun(scene);
    addEnvironment(scene);
    addBlock(scene, assets, assetRoot, "Floor", {0.0f, -0.1f, -10.0f}, {8.0f, 0.1f, 16.0f});
    addBlock(scene, assets, assetRoot, "Left Wall", {-8.5f, 1.5f, -10.0f}, {0.5f, 1.5f, 16.0f});
    addBlock(scene, assets, assetRoot, "Right Wall", {8.5f, 1.5f, -10.0f}, {0.5f, 1.5f, 16.0f});

    const Entity player = spawnFirstPersonPlayer(scene, {0.0f, 1.0f, 4.0f});
    scene.registry().get<NameComponent>(player).name = "Player";
    const Entity camera = findCameraChild(scene, player);
    addModuleComponent(scene, camera, "hitscan_weapon",
                       {{"enabled", true},
                        {"range", 100.0},
                        {"damage", 25.0},
                        {"cooldown_seconds", 0.2},
                        {"fire_action", "Fire"},
                        {"impact_effect", impactPrefab}});

    const std::array<glm::vec3, targetCount> targets{glm::vec3{-4.0f, 1.0f, -6.0f}, glm::vec3{3.0f, 1.5f, -9.0f},
                                                     glm::vec3{-2.0f, 2.0f, -14.0f}, glm::vec3{5.0f, 1.0f, -18.0f},
                                                     glm::vec3{0.0f, 2.5f, -22.0f}};
    for (size_t index = 0; index < targets.size(); ++index) {
        const Entity target =
            addBlock(scene, assets, assetRoot, "Target " + std::to_string(index + 1), targets[index], glm::vec3(0.5f));
        scene.registry().emplace<TagsComponent>(target).tags = {"target"};
        addModuleComponent(scene, target, "target", {{"enabled", true}, {"health", 50.0}});
    }

    const Entity exit = scene.createEntity("Exit");
    setLocal(scene, exit, glm::scale(glm::translate(glm::mat4(1.0f), {0.0f, 1.5f, -25.0f}), {4.0f, 1.5f, 1.0f}));
    scene.registry().emplace<ColliderComponent>(
        exit, ColliderComponent{.shape = BoxCollider{.halfExtents = glm::vec3(1.0f)}});
    scene.registry().emplace<RigidBodyComponent>(exit, RigidBodyComponent{.sensor = true});
    addModuleComponent(scene, exit, "goal_zone", {{"enabled", true}});
    scene.updateWorldTransforms();
}

// A one-shot burst of sparks: Effects derives its lifetime from lifetimeMax.
void buildImpact(Scene& scene) {
    const Entity impact = scene.createEntity("Impact");
    scene.registry().emplace<ParticleEmitterComponent>(
        impact, ParticleEmitterComponent{.maxParticles = 64,
                                         .spawnRate = 0.0f,
                                         .burstCount = 24,
                                         .lifetimeMin = 0.15f,
                                         .lifetimeMax = 0.4f,
                                         .coneAngleRadians = 1.2f,
                                         .speedMin = 2.0f,
                                         .speedMax = 5.0f,
                                         .gravity = {0.0f, -9.8f, 0.0f},
                                         .startSize = 0.06f,
                                         .endSize = 0.0f,
                                         .startColor = {1.0f, 0.8f, 0.3f, 1.0f},
                                         .endColor = {1.0f, 0.3f, 0.1f, 0.0f},
                                         .emissiveIntensity = 4.0f,
                                         .blendMode = ParticleBlendMode::Additive});
}

int writeTemplate(const fs::path& templateDirectory) {
    const fs::path assetRoot = templateDirectory / "assets";
    // The cube model is copied into each new project; the writer reads it
    // from the engine's assets through a scratch asset root.
    const fs::path scratch = fs::temp_directory_path() / "zenith_fps_template_writer";
    fs::remove_all(scratch);
    fs::create_directories(scratch / "primitives");
    fs::copy_file(fs::path(ZENITH_ASSET_DIR) / "primitives" / "cube.gltf", scratch / "primitives" / "cube.gltf");
    AssetManager assets;
    Scene scene;
    buildScene(scene, assets, scratch);
    const SceneDocumentResult savedScene =
        SceneDocument::save(templateDirectory / sceneFile, scene, assets, {.assetRoot = scratch});
    expect(static_cast<bool>(savedScene), "writer: the scene saves: " + savedScene.error);

    Scene prefabScene;
    buildImpact(prefabScene);
    PrefabSystem prefabs(assets, {.assetRoot = assetRoot});
    PrefabCapture capture;
    const Entity root = prefabScene.registry().view<ParticleEmitterComponent>().front();
    expect(static_cast<bool>(prefabs.capture(prefabScene, root, fs::path(impactPrefab), capture)) &&
               static_cast<bool>(savePrefab(assetRoot / impactPrefab, capture.document)),
           "writer: the impact prefab saves");
    fs::remove_all(scratch);
    std::cout << "Wrote " << (templateDirectory / sceneFile).string() << " and " << (assetRoot / impactPrefab).string()
              << '\n';
    return failures == 0 ? 0 : 1;
}

size_t countNamed(const Scene& scene, std::string_view prefix) {
    size_t count = 0;
    for (const auto [entity, name] : scene.registry().view<const NameComponent>().each()) {
        count += name.name.starts_with(prefix) ? 1 : 0;
    }
    return count;
}

// Whether the named entity (or, with an empty name, a target) keeps a module
// component with key.
bool hasModuleComponent(const Scene& scene, std::string_view entityName, std::string_view key) {
    for (const auto [entity, name, preserved] :
         scene.registry().view<const NameComponent, const PreservedComponentsComponent>().each()) {
        if (name.name == entityName || (entityName.empty() && name.name.starts_with("Target"))) {
            for (const auto& [component, json] : preserved.components) {
                if (component == key) {
                    return true;
                }
            }
        }
    }
    return false;
}

void testCommittedContent() {
    const fs::path directory = fs::path(ZENITH_TEMPLATE_DIR) / fpsGameTemplateName;
    ProjectTemplateInfo info;
    expect(readProjectTemplateInfo(directory, info) && info.name == "First-Person Shooter" &&
               info.startupScene == fs::path(sceneFile) && info.inputActions == "input_actions.json" &&
               info.tags == std::vector<std::string>{"target"},
           "template.json names the scene, input actions, and tags");

    AssetManager assets;
    Scene scene;
    const SceneDocumentResult loaded =
        SceneDocument::load(directory / sceneFile, assets, {.assetRoot = fs::path(ZENITH_ASSET_DIR)}, scene);
    expect(static_cast<bool>(loaded), "the starter scene loads with the engine's types: " + loaded.error);
    expect(scene.registry().view<const CharacterControllerComponent>().size() == 1 &&
               scene.registry().view<const FirstPersonCameraComponent>().size() == 1,
           "the scene has one first-person player");
    expect(countNamed(scene, "Target ") == targetCount && findEntitiesWithTag(scene, "target").size() == targetCount,
           "the scene has the tagged targets");
    const auto cameras = scene.registry().view<const FirstPersonCameraComponent>();
    const auto* cameraModules =
        cameras.empty() ? nullptr : scene.registry().try_get<PreservedComponentsComponent>(cameras.front());
    expect(cameraModules != nullptr &&
               std::ranges::any_of(cameraModules->components,
                                   [](const auto& entry) { return entry.first == "hitscan_weapon"; }),
           "the player's camera carries the hitscan weapon");
    expect(hasModuleComponent(scene, "", "target") && hasModuleComponent(scene, "Exit", "goal_zone"),
           "targets and the exit carry the module's behaviours");

    PrefabSystem prefabs(assets, {.assetRoot = directory / "assets"});
    PrefabDocument impact;
    expect(prefabs.load(fs::path(impactPrefab), impact) && impact.entities.size() == 1,
           "the impact prefab loads with one entity");

    InputActionMap actions;
    expect(loadInputActionMap(directory / "input_actions.json", actions) && actions.contains("Fire") &&
               actions.contains("Pause") && actions.contains("Move") && actions.contains("Look"),
           "the input actions load and include Fire and Pause");
}

void testGeneratedProject(const fs::path& root) {
    AssetManager assets;
    const ProjectTemplateResult created =
        generateProjectFromTemplate({.displayName = "Range Day",
                                     .parentDirectory = root,
                                     .templateDirectory = fs::path(ZENITH_TEMPLATE_DIR) / fpsGameTemplateName,
                                     .engineRoot = ZENITH_ENGINE_BUILD_DIR,
                                     .engineAssetRoot = ZENITH_ASSET_DIR},
                                    assets);
    expect(static_cast<bool>(created), "an FPS project generates: " + created.error);
    if (!created) {
        return;
    }
    const fs::path directory = created.projectFile.parent_path();
    ProjectConfig project;
    expect(loadProjectConfig(created.projectFile, project) &&
               project.startupScene == directory / "scenes" / "main.zenith-scene" &&
               project.inputActions == directory / "input_actions.json" &&
               project.tags == std::vector<std::string>{"target"},
           "the project names the template's scene, input actions, and tags");
    expect(!fs::exists(directory / templateInfoFileName) && !fs::exists(directory / templateSnippetDirectoryName),
           "template.json and snippets stay in the template");
    AssetManager sceneAssets;
    Scene scene;
    expect(static_cast<bool>(loadProjectStartupScene(project, sceneAssets, scene)) &&
               fs::is_regular_file(directory / "assets" / "prefabs" / "impact.prefab"),
           "the project's scene loads against its own assets, and the impact prefab is copied");
    InputActionMap actions;
    expect(loadProjectInputActions(project, actions) && actions.contains("Fire"), "the project's input actions load");
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string_view(argv[1]) == "--write") {
        return writeTemplate(fs::absolute(argv[2]));
    }
    const fs::path root = fs::temp_directory_path() / "zenith_fps_template_tests";
    std::error_code error;
    fs::remove_all(root, error);
    testCommittedContent();
    testGeneratedProject(root);
    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " FPS template test(s) failed\n";
        return 1;
    }
    std::cout << "FPS template tests passed\n";
    return 0;
}
