#include "engine/project/ProjectConfig.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_project_config_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory / "content", error);
    std::filesystem::create_directories(directory / "scenes" / "chapter", error);

    const std::filesystem::path projectPath = directory / "sample.zenith-project";
    ProjectConfig source{.projectFile = projectPath,
                         .displayName = "Sample project",
                         .assetRoot = directory / "content",
                         .sceneRoot = directory / "scenes",
                         .startupScene = directory / "scenes" / "room.zenith-scene"};
    expect(saveProjectConfig(projectPath, source).success, "project configuration should save");

    ProjectConfig loaded;
    expect(loadProjectConfig(projectPath, loaded).success, "project configuration should load");
    expect(loaded.displayName == source.displayName && loaded.assetRoot == source.assetRoot &&
               loaded.sceneRoot == source.sceneRoot && loaded.startupScene == source.startupScene,
           "project fields should round-trip as paths relative to the project file");

    std::filesystem::path resolvedScene;
    expect(resolveProjectScene(loaded, "chapter/room.zenith-scene", resolvedScene).success &&
               resolvedScene == directory / "scenes" / "chapter" / "room.zenith-scene",
           "scene references resolve beneath the configured scene root");
    expect(!resolveProjectScene(loaded, "../outside.zenith-scene", resolvedScene),
           "scene references cannot escape the configured scene root");

    AssetManager assets;
    Scene authoredScene;
    authoredScene.createEntity("Authored startup entity");
    expect(SceneDocument::save(source.startupScene, authoredScene, assets, {.assetRoot = source.assetRoot}).success,
           "the authored startup fixture should save");
    Scene runtimeScene;
    expect(loadProjectStartupScene(loaded, assets, runtimeScene).success,
           "the configured startup scene should load through the shared project contract");
    expect(runtimeScene.registry().view<NameComponent>().size() == 1,
           "startup scene loading should replace the runtime scene with authored content");

    authoredScene.createEntity("Second authored entity");
    const std::filesystem::path chapterScene = directory / "scenes" / "chapter" / "room.zenith-scene";
    expect(SceneDocument::save(chapterScene, authoredScene, assets, {.assetRoot = source.assetRoot}).success,
           "the transition scene fixture should save");
    expect(loadProjectScene(loaded, "chapter/room.zenith-scene", assets, runtimeScene).success &&
               runtimeScene.registry().view<NameComponent>().size() == 2,
           "project-relative scene references load authored transition destinations");

    const std::filesystem::path legacyPath = directory / "legacy.zenith-project";
    writeFile(legacyPath, R"({"version":1,"assetRoot":"content","sceneRoot":"scenes"})");
    ProjectConfig legacy;
    expect(loadProjectConfig(legacyPath, legacy).success && legacy.displayName == "legacy" &&
               legacy.startupScene.empty(),
           "version 1 projects load with a derived display name and no startup scene");
    Scene unchangedScene;
    const Entity unchangedEntity = unchangedScene.createEntity("Keep me");
    expect(!loadProjectStartupScene(legacy, assets, unchangedScene),
           "a legacy project without a startup scene fails with a recoverable error");
    expect(unchangedScene.registry().valid(unchangedEntity),
           "a missing startup scene does not mutate the destination scene");

    const std::filesystem::path escapingPath = directory / "escaping.zenith-project";
    writeFile(
        escapingPath,
        R"({"version":2,"displayName":"Escape","assetRoot":"content","sceneRoot":"scenes","startupScene":"../outside.zenith-scene"})");
    ProjectConfig escaping = loaded;
    expect(!loadProjectConfig(escapingPath, escaping), "project startup scenes cannot escape the scene root");
    expect(escaping.displayName == loaded.displayName,
           "failed containment validation preserves the active project configuration");

    writeFile(projectPath, "{not json");
    ProjectConfig preserved = loaded;
    expect(!loadProjectConfig(projectPath, preserved), "malformed project configuration should fail");
    expect(preserved.assetRoot == loaded.assetRoot, "failed project load should preserve the active configuration");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
