#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ProjectConfig.hpp"

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
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_project_asset_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory / "content", error);
    std::filesystem::create_directories(directory / "scenes", error);

    const std::filesystem::path projectPath = directory / "sample.zenith-project";
    ProjectConfig source{
        .projectFile = projectPath, .assetRoot = directory / "content", .sceneRoot = directory / "scenes"};
    expect(saveProjectConfig(projectPath, source).success, "project configuration should save");

    ProjectConfig loaded;
    expect(loadProjectConfig(projectPath, loaded).success, "project configuration should load");
    expect(loaded.assetRoot == source.assetRoot && loaded.sceneRoot == source.sceneRoot,
           "project roots should round-trip as paths relative to the project file");

    writeFile(directory / "content" / "model.gltf", "{}");
    writeFile(directory / "content" / "albedo.PNG", "png");
    writeFile(directory / "content" / "ignored.txt", "ignored");
    writeFile(directory / "scenes" / "room.zenith-scene", "{}");

    AssetCatalog catalog(loaded.assetRoot, loaded.sceneRoot);
    AssetCatalogResult refresh = catalog.refresh();
    expect(refresh.success && catalog.assets().size() == 3 && refresh.changes.size() == 3,
           "initial catalog refresh should discover supported project assets");
    expect(catalog.refresh().changes.empty(), "an unchanged catalog refresh should not report changes");

    writeFile(directory / "content" / "model.gltf", "{\"changed\":true}");
    std::filesystem::remove(directory / "content" / "albedo.PNG", error);
    writeFile(directory / "content" / "sound.ogg", "ogg");
    refresh = catalog.refresh();
    size_t added = 0;
    size_t modified = 0;
    size_t removed = 0;
    for (const AssetCatalogChange& change : refresh.changes) {
        added += change.kind == AssetChangeKind::Added;
        modified += change.kind == AssetChangeKind::Modified;
        removed += change.kind == AssetChangeKind::Removed;
    }
    expect(refresh.success && added == 1 && modified == 1 && removed == 1,
           "catalog refresh should distinguish added, modified, and removed assets");

    writeFile(projectPath, "{not json");
    ProjectConfig preserved = loaded;
    expect(!loadProjectConfig(projectPath, preserved).success, "malformed project configuration should fail");
    expect(preserved.assetRoot == loaded.assetRoot, "failed project load should preserve the active configuration");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
