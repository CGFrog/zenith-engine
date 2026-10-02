#include "editor/document/DefaultScene.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

Entity findNamed(const Scene& scene, std::string_view name) {
    const auto names = scene.registry().view<const NameComponent>();
    for (Entity entity : names) {
        if (names.get<const NameComponent>(entity).name == name) {
            return entity;
        }
    }
    return entt::null;
}

void expectDefaultContent(const Scene& scene, const char* context) {
    const Entity sun = findNamed(scene, "Sun");
    const Entity environment = findNamed(scene, "Environment");
    const Entity cube = findNamed(scene, "Cube");
    if (sun == entt::null || environment == entt::null || cube == entt::null) {
        std::cerr << "FAILED: " << context << ": default entities are missing\n";
        ++failures;
        return;
    }
    expect(scene.registry().all_of<DirectionalLightComponent>(sun), "the sun should carry a directional light");
    expect(scene.registry().all_of<EnvironmentLightComponent>(environment),
           "the environment entity should carry the sky environment");
    expect(scene.registry().all_of<MeshRendererComponent>(cube), "the cube should render a mesh");
}
} // namespace

int main() {
    const std::filesystem::path assetRoot = ZENITH_ASSET_DIR;
    {
        AssetManager assets;
        Scene scene;
        const SceneResult result = populateDefaultScene(scene, assets, assetRoot);
        expect(result.success, "the default scene should load the shipped cube");
        expectDefaultContent(scene, "populated scene");

        // The cube must reference a project asset so the scene survives a save.
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_default_scene_tests";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const std::filesystem::path path = directory / "default.zenith-scene";
        const SceneDocumentOptions options{.assetRoot = assetRoot};
        expect(SceneDocument::save(path, scene, assets, options).success, "the default scene should save");
        Scene reloaded;
        expect(SceneDocument::load(path, assets, options, reloaded).success, "the saved default scene should load");
        expectDefaultContent(reloaded, "reloaded scene");
        std::filesystem::remove_all(directory, error);
    }
    {
        AssetManager assets;
        Scene scene;
        const SceneResult result =
            populateDefaultScene(scene, assets, std::filesystem::temp_directory_path() / "zenith_missing_asset_root");
        expect(!result.success && !result.error.empty(), "a missing cube should be reported");
        expect(findNamed(scene, "Sun") != entt::null && findNamed(scene, "Environment") != entt::null,
               "lighting should still be added when the cube is unavailable");
        expect(findNamed(scene, "Cube") == entt::null, "no cube entity should exist without its mesh");
    }
    return failures == 0 ? 0 : 1;
}
