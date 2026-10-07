// The FPS project template (templates/fps_game): its committed starter scene
// and impact prefab are valid documents with the expected content, and a
// project generated from it names its scene, input actions, and tags.
//
// zenith_fps_template_writer (FpsTemplateWriter.cpp) writes the scene and
// prefab with the engine's own serializers; run it after changing what they
// hold, then commit the result.

#include "FpsTemplateContent.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "engine/input/InputActionsJson.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/scene/Tags.hpp"
#include "support/Doctest.hpp"
#include "support/TempDirectory.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using fpstemplate::impactPrefab;
using fpstemplate::sceneFile;
using fpstemplate::targetCount;

namespace {
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
} // namespace

TEST_SUITE_BEGIN("FpsTemplate");

TEST_CASE("FpsTemplate: the committed scene prefab and input actions hold the template's content") {
    const fs::path directory = fs::path(ZENITH_TEMPLATE_DIR) / fpsGameTemplateName;
    ProjectTemplateInfo info;
    CHECK_MESSAGE((readProjectTemplateInfo(directory, info) && info.name == "First-Person Shooter" &&
                   info.startupScene == fs::path(sceneFile) && info.inputActions == "input_actions.json" &&
                   info.tags == std::vector<std::string>{"target"}),
                  "template.json names the scene, input actions, and tags");

    AssetManager assets;
    Scene scene;
    const SceneDocumentResult loaded =
        SceneDocument::load(directory / sceneFile, assets, {.assetRoot = fs::path(ZENITH_ASSET_DIR)}, scene);
    CHECK_MESSAGE(static_cast<bool>(loaded), ("the starter scene loads with the engine's types: " + loaded.error));
    CHECK_MESSAGE((scene.registry().view<const CharacterControllerComponent>().size() == 1 &&
                   scene.registry().view<const FirstPersonCameraComponent>().size() == 1),
                  "the scene has one first-person player");
    CHECK_MESSAGE(
        (countNamed(scene, "Target ") == targetCount && findEntitiesWithTag(scene, "target").size() == targetCount),
        "the scene has the tagged targets");
    const auto cameras = scene.registry().view<const FirstPersonCameraComponent>();
    const auto* cameraModules =
        cameras.empty() ? nullptr : scene.registry().try_get<PreservedComponentsComponent>(cameras.front());
    CHECK_MESSAGE((cameraModules != nullptr &&
                   std::ranges::any_of(cameraModules->components,
                                       [](const auto& entry) { return entry.first == "hitscan_weapon"; })),
                  "the player's camera carries the hitscan weapon");
    CHECK_MESSAGE((hasModuleComponent(scene, "", "target") && hasModuleComponent(scene, "Exit", "goal_zone")),
                  "targets and the exit carry the module's behaviours");

    PrefabSystem prefabs(assets, {.assetRoot = directory / "assets"});
    PrefabDocument impact;
    CHECK_MESSAGE((prefabs.load(fs::path(impactPrefab), impact) && impact.entities.size() == 1),
                  "the impact prefab loads with one entity");

    InputActionMap actions;
    CHECK_MESSAGE((loadInputActionMap(directory / "input_actions.json", actions) && actions.contains("Fire") &&
                   actions.contains("Pause") && actions.contains("Move") && actions.contains("Look")),
                  "the input actions load and include Fire and Pause");
}

TEST_CASE("FpsTemplate: a generated project names the template's scene input actions and tags") {
    const testsupport::TempDirectory root("fps-template-generate");
    AssetManager assets;
    const ProjectTemplateResult created =
        generateProjectFromTemplate({.displayName = "Range Day",
                                     .parentDirectory = root.path(),
                                     .templateDirectory = fs::path(ZENITH_TEMPLATE_DIR) / fpsGameTemplateName,
                                     .engineRoot = ZENITH_ENGINE_BUILD_DIR,
                                     .engineAssetRoot = ZENITH_ASSET_DIR},
                                    assets);
    REQUIRE_MESSAGE(static_cast<bool>(created), ("an FPS project generates: " + created.error));
    const fs::path directory = created.projectFile.parent_path();
    ProjectConfig project;
    CHECK_MESSAGE((loadProjectConfig(created.projectFile, project) &&
                   project.startupScene == directory / "scenes" / "main.zenith-scene" &&
                   project.inputActions == directory / "input_actions.json" &&
                   project.tags == std::vector<std::string>{"target"}),
                  "the project names the template's scene, input actions, and tags");
    CHECK_MESSAGE(
        (!fs::exists(directory / templateInfoFileName) && !fs::exists(directory / templateSnippetDirectoryName)),
        "template.json and snippets stay in the template");
    AssetManager sceneAssets;
    Scene scene;
    CHECK_MESSAGE((static_cast<bool>(loadProjectStartupScene(project, sceneAssets, scene)) &&
                   fs::is_regular_file(directory / "assets" / "prefabs" / "impact.prefab")),
                  "the project's scene loads against its own assets, and the impact prefab is copied");
    InputActionMap actions;
    CHECK_MESSAGE((loadProjectInputActions(project, actions) && actions.contains("Fire")),
                  "the project's input actions load");
}

TEST_SUITE_END();
