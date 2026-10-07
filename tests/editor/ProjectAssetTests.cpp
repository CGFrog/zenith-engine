#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ReflectionProbeBaker.hpp"

#include "support/Doctest.hpp"

#include <filesystem>

using testsupport::readText;
using testsupport::TempDirectory;
using testsupport::writeText;

TEST_SUITE_BEGIN("ProjectAsset");

TEST_CASE("ProjectAsset: catalog refresh reports added modified and removed assets") {
    const TempDirectory directory("project-asset-catalog");
    std::error_code error;
    std::filesystem::create_directories(directory / "content", error);
    std::filesystem::create_directories(directory / "scenes", error);

    writeText(directory / "content" / "model.gltf", "{}");
    writeText(directory / "content" / "albedo.PNG", "png");
    writeText(directory / "content" / "ignored.txt", "ignored");

    writeText(directory / "scenes" / "room.zenith-scene", "{}");

    AssetCatalog catalog(directory / "content", directory / "scenes");
    AssetCatalogResult refresh = catalog.refresh();
    CHECK_MESSAGE((refresh.success && catalog.assets().size() == 3 && refresh.changes.size() == 3),
                  "initial catalog refresh should discover supported project assets");
    CHECK_MESSAGE(catalog.refresh().changes.empty(), "an unchanged catalog refresh should not report changes");

    writeText(directory / "content" / "model.gltf", "{\"changed\":true}");
    std::filesystem::remove(directory / "content" / "albedo.PNG", error);
    writeText(directory / "content" / "sound.ogg", "ogg");
    refresh = catalog.refresh();
    size_t added = 0;
    size_t modified = 0;
    size_t removed = 0;
    for (const AssetCatalogChange& change : refresh.changes) {
        added += change.kind == AssetCatalogChangeKind::Added;
        modified += change.kind == AssetCatalogChangeKind::Modified;
        removed += change.kind == AssetCatalogChangeKind::Removed;
    }
    CHECK_MESSAGE((refresh.success && added == 1 && modified == 1 && removed == 1),
                  "catalog refresh should distinguish added, modified, and removed assets");
}

TEST_CASE("ProjectAsset: reflection probe baking is deterministic and validates its policy") {
    const TempDirectory directory("project-asset-probe");
    TextureAsset environment;
    environment.width = 2;
    environment.height = 2;
    environment.rgba = {255, 128, 32, 255, 128, 64, 16, 255, 16, 32, 64, 255, 32, 64, 128, 255};
    ReflectionProbeBaker baker;
    const auto bakePath = directory / "derived" / "probe.zenith-environment";
    const ReflectionProbeBakeResult firstBake = baker.bake(environment, bakePath, {.roughnessLevels = 4});
    const std::string firstBakeContents = readText(bakePath);
    const ReflectionProbeBakeResult secondBake = baker.bake(environment, bakePath, {.roughnessLevels = 4});
    CHECK_MESSAGE((firstBake && secondBake && firstBake.sourceHash == secondBake.sourceHash &&
                   readText(bakePath) == firstBakeContents),
                  "editor probe baking should produce deterministic reusable prefiltered data");
    CHECK_MESSAGE(!baker.bake(environment, bakePath, {.roughnessLevels = 0}),
                  "editor probe baking should reject invalid authoring policy");
}

TEST_SUITE_END();
