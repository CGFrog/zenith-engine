#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ReflectionProbeBaker.hpp"

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

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_project_asset_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory / "content", error);
    std::filesystem::create_directories(directory / "scenes", error);

    writeFile(directory / "content" / "model.gltf", "{}");
    writeFile(directory / "content" / "albedo.PNG", "png");
    writeFile(directory / "content" / "ignored.txt", "ignored");

    writeFile(directory / "scenes" / "room.zenith-scene", "{}");

    AssetCatalog catalog(directory / "content", directory / "scenes");
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
        added += change.kind == AssetCatalogChangeKind::Added;
        modified += change.kind == AssetCatalogChangeKind::Modified;
        removed += change.kind == AssetCatalogChangeKind::Removed;
    }
    expect(refresh.success && added == 1 && modified == 1 && removed == 1,
           "catalog refresh should distinguish added, modified, and removed assets");

    TextureAsset environment;
    environment.width = 2;
    environment.height = 2;
    environment.rgba = {255, 128, 32, 255, 128, 64, 16, 255, 16, 32, 64, 255, 32, 64, 128, 255};
    ReflectionProbeBaker baker;
    const auto bakePath = directory / "derived" / "probe.zenith-environment";
    const ReflectionProbeBakeResult firstBake = baker.bake(environment, bakePath, {.roughnessLevels = 4});
    const std::string firstBakeContents = readFile(bakePath);
    const ReflectionProbeBakeResult secondBake = baker.bake(environment, bakePath, {.roughnessLevels = 4});
    expect(firstBake && secondBake && firstBake.sourceHash == secondBake.sourceHash &&
               readFile(bakePath) == firstBakeContents,
           "editor probe baking should produce deterministic reusable prefiltered data");
    expect(!baker.bake(environment, bakePath, {.roughnessLevels = 0}),
           "editor probe baking should reject invalid authoring policy");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
