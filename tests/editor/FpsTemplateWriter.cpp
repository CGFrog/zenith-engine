// Writes the FPS project template's starter scene and impact prefab
// (templates/fps_game) with the engine's own serializers, so they stay
// documents the engine produces. Not a test: run
// zenith_fps_template_writer <template folder> after changing what they hold,
// then commit the result; FpsTemplateTests.cpp checks what is committed.

#include "FpsTemplateContent.hpp"
#include "editor/document/DefaultScene.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/ParticleEmitter.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/scene/Tags.hpp"
#include "support/StandaloneChecks.hpp"
#include "support/TempDirectory.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using fpstemplate::impactPrefab;
using fpstemplate::sceneFile;
using fpstemplate::targetCount;

namespace {
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
Entity addBlock(testsupport::StandaloneChecks& checks, Scene& scene, AssetManager& assets, const fs::path& assetRoot,
                std::string name, const glm::vec3& position, const glm::vec3& halfSize) {
    Entity block = entt::null;
    if (!addCube(scene, assets, assetRoot, block)) {
        checks.check(false, "the cube model loads");
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
void buildScene(testsupport::StandaloneChecks& checks, Scene& scene, AssetManager& assets, const fs::path& assetRoot) {
    addSun(scene);
    addEnvironment(scene);
    addBlock(checks, scene, assets, assetRoot, "Floor", {0.0f, -0.1f, -10.0f}, {8.0f, 0.1f, 16.0f});
    addBlock(checks, scene, assets, assetRoot, "Left Wall", {-8.5f, 1.5f, -10.0f}, {0.5f, 1.5f, 16.0f});
    addBlock(checks, scene, assets, assetRoot, "Right Wall", {8.5f, 1.5f, -10.0f}, {0.5f, 1.5f, 16.0f});

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
        const Entity target = addBlock(checks, scene, assets, assetRoot, "Target " + std::to_string(index + 1),
                                       targets[index], glm::vec3(0.5f));
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
    testsupport::StandaloneChecks checks;
    const fs::path assetRoot = templateDirectory / "assets";
    // The cube model is copied into each new project; the writer reads it
    // from the engine's assets through a scratch asset root.
    const testsupport::TempDirectory scratch("fps-template-writer");
    fs::create_directories(scratch / "primitives");
    fs::copy_file(fs::path(ZENITH_ASSET_DIR) / "primitives" / "cube.gltf", scratch / "primitives" / "cube.gltf");
    AssetManager assets;
    Scene scene;
    buildScene(checks, scene, assets, scratch.path());
    const SceneDocumentResult savedScene =
        SceneDocument::save(templateDirectory / sceneFile, scene, assets, {.assetRoot = scratch.path()});
    checks.check(static_cast<bool>(savedScene), "the scene saves: " + savedScene.error);

    Scene prefabScene;
    buildImpact(prefabScene);
    PrefabSystem prefabs(assets, {.assetRoot = assetRoot});
    PrefabCapture capture;
    const Entity root = prefabScene.registry().view<ParticleEmitterComponent>().front();
    checks.check(static_cast<bool>(prefabs.capture(prefabScene, root, fs::path(impactPrefab), capture)) &&
                     static_cast<bool>(savePrefab(assetRoot / impactPrefab, capture.document)),
                 "the impact prefab saves");
    std::cout << "Wrote " << (templateDirectory / sceneFile).string() << " and " << (assetRoot / impactPrefab).string()
              << '\n';
    return checks.exitCode("FPS template writer");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zenith_fps_template_writer <template folder, such as templates/fps_game>\n";
        return 2;
    }
    return writeTemplate(fs::absolute(argv[1]));
}
