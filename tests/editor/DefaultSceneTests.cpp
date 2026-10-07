#include "editor/document/DefaultScene.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <string>
#include <string_view>

TEST_SUITE_BEGIN("DefaultScene");

namespace {

Entity findNamed(const Scene& scene, std::string_view name) {
    const auto names = scene.registry().view<const NameComponent>();
    for (Entity entity : names) {
        if (names.get<const NameComponent>(entity).name == name) {
            return entity;
        }
    }
    return entt::null;
}

void checkDefaultContent(const Scene& scene, const char* context) {
    const Entity sun = findNamed(scene, "Sun");
    const Entity environment = findNamed(scene, "Environment");
    const Entity cube = findNamed(scene, "Cube");
    const bool present = sun != entt::null && environment != entt::null && cube != entt::null;
    CHECK_MESSAGE(present, (std::string(context) + ": default entities are missing"));
    if (!present) {
        return;
    }
    CHECK_MESSAGE(scene.registry().all_of<DirectionalLightComponent>(sun), "the sun should carry a directional light");
    CHECK_MESSAGE(scene.registry().all_of<EnvironmentLightComponent>(environment),
                  "the environment entity should carry the sky environment");
    CHECK_MESSAGE(scene.registry().all_of<MeshRendererComponent>(cube), "the cube should render a mesh");
}

} // namespace

TEST_CASE("DefaultScene: the default scene loads the shipped content and survives a save") {
    const std::filesystem::path assetRoot = ZENITH_ASSET_DIR;
    AssetManager assets;
    Scene scene;
    const SceneResult result = populateDefaultScene(scene, assets, assetRoot);
    CHECK_MESSAGE(result.success, "the default scene should load the shipped cube");
    checkDefaultContent(scene, "populated scene");

    // The cube must reference a project asset so the scene survives a save.
    const testsupport::TempDirectory directory("default-scene-save");
    const std::filesystem::path path = directory / "default.zenith-scene";
    const SceneDocumentOptions options{.assetRoot = assetRoot};
    CHECK_MESSAGE(SceneDocument::save(path, scene, assets, options).success, "the default scene should save");
    Scene reloaded;
    CHECK_MESSAGE(SceneDocument::load(path, assets, options, reloaded).success, "the saved default scene should load");
    checkDefaultContent(reloaded, "reloaded scene");
}

TEST_CASE("DefaultScene: a missing cube is reported and the lighting is still added") {
    const testsupport::TempDirectory directory("default-scene-missing");
    AssetManager assets;
    Scene scene;
    const SceneResult result = populateDefaultScene(scene, assets, directory / "missing_asset_root");
    CHECK_MESSAGE((!result.success && !result.error.empty()), "a missing cube should be reported");
    CHECK_MESSAGE((findNamed(scene, "Sun") != entt::null && findNamed(scene, "Environment") != entt::null),
                  "lighting should still be added when the cube is unavailable");
    CHECK_MESSAGE((findNamed(scene, "Cube") == entt::null), "no cube entity should exist without its mesh");
}

TEST_SUITE_END();
