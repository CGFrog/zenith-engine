// Asset import and management in the Asset Browser: copying files in (by
// import and OS drop), import settings and reimport, reference scans and the
// delete guard, and definition tables as data assets.

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/FileDropQueue.hpp"
#include "editor/panels/AssetBrowserPanel.hpp"
#include "editor/panels/DataTableView.hpp"
#include "editor/project/AssetCatalog.hpp"
#include "editor/project/AssetImport.hpp"
#include "editor/project/AssetReferences.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "engine/assets/AssetMeta.hpp"
#include "engine/gameplay/DefinitionSchemas.hpp"

#include "support/Doctest.hpp"
#include "support/Files.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_BEGIN("AssetImport");

namespace {

bool hasMessage(const EditorDiagnostics& diagnostics, std::string_view text, EditorDiagnosticSeverity severity) {
    return std::ranges::any_of(diagnostics.entries(), [&](const EditorDiagnostic& entry) {
        return entry.severity == severity && entry.message.find(text) != std::string::npos;
    });
}

// Headless ImGui: no renderer, just enough state for widgets to run.
class HeadlessImGui {
  public:
    HeadlessImGui() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~HeadlessImGui() {
        ImGui::DestroyContext();
    }

    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    void frame(const std::function<void()>& draw) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({800.0f, 600.0f});
        ImGui::Begin("Asset Browser");
        draw();
        ImGui::End();
        ImGui::Render();
    }
};

// A project with an asset root and a separate scene root, owned by one test.
struct TestProject {
    testsupport::TempDirectory root;
    std::filesystem::path directory;
    std::filesystem::path content;
    std::filesystem::path scenes;

    explicit TestProject(std::string_view name)
        : root(name)
        , directory(root.path())
        , content(directory / "content")
        , scenes(directory / "scenes") {
        std::filesystem::create_directories(content);
        std::filesystem::create_directories(scenes);
    }

    TestProject(const TestProject&) = delete;
    TestProject& operator=(const TestProject&) = delete;

    std::filesystem::path settingsFile() const {
        return directory / "settings" / "asset-browser.json";
    }
};

TEST_CASE("AssetImport: import into folder") {
    TestProject project("asset-import-copy");
    const std::filesystem::path outside = project.directory / "outside";
    testsupport::writeText(outside / "stone.png", "stone");
    testsupport::writeText(outside / "stone.png.meta", "{}");
    testsupport::writeText(outside / "pack" / "a.wav", "a");
    testsupport::writeText(outside / "pack" / "a.wav.meta", "{}");
    testsupport::writeText(outside / "pack" / ".hidden", "h");
    testsupport::writeText(project.content / "stone.png", "existing");

    const std::array<std::filesystem::path, 3> sources{outside / "stone.png", outside / "pack",
                                                       outside / "stone.png.meta"};
    const AssetImportResult result = importIntoFolder(sources, project.content);
    CHECK_MESSAGE((result.success() && result.imported.size() == 2), "a file and a folder should be imported");
    CHECK_MESSAGE((std::filesystem::exists(project.content / "stone (1).png") &&
                   testsupport::readText(project.content / "stone.png") == "existing"),
                  "a taken name should get ' (1)' and leave the existing file alone");
    CHECK_MESSAGE((std::filesystem::exists(project.content / "pack" / "a.wav") &&
                   !std::filesystem::exists(project.content / "pack" / "a.wav.meta") &&
                   !std::filesystem::exists(project.content / "pack" / ".hidden")),
                  "a folder's sidecars and hidden files should stay behind");
    CHECK_MESSAGE(uniqueImportPath(project.content, "stone.png").filename() == "stone (2).png",
                  "the next copy should count on");
    CHECK_MESSAGE(uniqueImportPath(project.content, "pack").filename() == "pack (1)", "folders have no extension");

    const std::array<std::filesystem::path, 2> invalid{outside / "missing.png", project.directory};
    const AssetImportResult failed = importIntoFolder(invalid, project.content);
    CHECK_MESSAGE((failed.errors.size() == 2 && failed.imported.empty()),
                  "a missing source and a folder containing the destination should be refused");
}

TEST_CASE("AssetImport: project import and settings") {
    TestProject project("asset-import-project");
    const std::filesystem::path outside = project.directory / "outside";
    testsupport::writeText(outside / "wall.png", "png");
    testsupport::writeText(project.content / "sounds" / "hit.wav", "wav");
    testsupport::writeText(project.scenes / "level.zenith-scene", "{}");

    std::vector<std::filesystem::path> reloadedTextures;
    std::vector<std::filesystem::path> reloadedSounds;
    const ProjectAssetActions actions{
        .reloadTexture = [&](const std::filesystem::path& path) { reloadedTextures.push_back(path); },
        .reloadAudio = [&](const std::filesystem::path& path) { reloadedSounds.push_back(path); },
    };
    EditorDiagnostics diagnostics;
    ProjectAssets assets(project.content, project.scenes, project.settingsFile(),
                         AssetCatalogOptions{.watchFiles = false});
    assets.pollChanges(diagnostics, actions);

    const std::array<std::filesystem::path, 1> sources{outside / "wall.png"};
    const std::vector<std::filesystem::path> imported = assets.importFiles(sources, project.content, diagnostics);
    const std::filesystem::path wall = normalizedCatalogPath(project.content / "wall.png");
    const EditorAssetRecord* record = assets.catalog().findAsset(wall);
    CHECK_MESSAGE((imported.size() == 1 && imported.front() == wall && record != nullptr && record->guid),
                  "an imported file should be listed with a GUID");
    CHECK_MESSAGE(std::filesystem::exists(assetMetaPath(wall)), "an imported file should get a .meta sidecar");
    CHECK_MESSAGE(hasMessage(diagnostics, "Imported 'wall.png'", EditorDiagnosticSeverity::Info),
                  "the import should be reported");
    CHECK_MESSAGE((assets.importFiles(sources, project.directory, diagnostics).empty() &&
                   hasMessage(diagnostics, "Cannot import into", EditorDiagnosticSeverity::Error)),
                  "importing outside the project's folders should be refused");

    CHECK_MESSAGE(assets.importSettings(wall) == nlohmann::json::object(), "a new asset should have empty settings");
    CHECK_MESSAGE(assets.setImportSettings(wall, {{"usage", "normal"}}, diagnostics), "settings should be stored");
    CHECK_MESSAGE((assets.importSettings(wall) == nlohmann::json{{"usage", "normal"}}),
                  "stored settings should read back");
    AssetMeta meta;
    CHECK_MESSAGE((readAssetMeta(assetMetaPath(wall), meta) && meta.importSettings["usage"] == "normal"),
                  "the settings should be written to the sidecar");
    assets.pollChanges(diagnostics, actions);
    CHECK_MESSAGE((reloadedTextures.size() == 1 && reloadedTextures.front() == wall),
                  "applying settings should reimport the texture");

    const std::filesystem::path hit = normalizedCatalogPath(project.content / "sounds" / "hit.wav");
    CHECK_MESSAGE(assets.queueReimport(hit), "a sound should be reimportable");
    CHECK_MESSAGE(!assets.queueReimport(normalizedCatalogPath(project.scenes / "level.zenith-scene")),
                  "a scene has nothing to reimport");
    assets.pollChanges(diagnostics, actions);
    CHECK_MESSAGE((reloadedSounds.size() == 1 && reloadedSounds.front() == hit), "Reimport should reload the sound");
}

void writeReferencingProject(const TestProject& project) {
    testsupport::writeText(project.content / "textures" / "plate.png", "png");
    testsupport::writeText(project.content / "textures" / "unused.png", "png");
    testsupport::writeText(project.content / "sounds" / "hit.wav", "wav");
    testsupport::writeText(project.content / "materials" / "plate.zmat",
                           R"({"version": 1, "base_color_texture": {"path": "../textures/plate.png"}})");
    testsupport::writeText(project.scenes / "level.zenith-scene", R"({"version": 2, "entities": [
        {"id": 7, "components": {"decal": {"albedo_asset": {"path": "textures/plate.png"}}}},
        {"id": 8, "components": {"audio_emitter": {"clip_asset": "sounds/hit.wav"}}}]})");
    testsupport::writeText(project.content / "prefabs" / "crate.prefab",
                           R"({"version": 1, "entities": [], "nested": []})");
    testsupport::writeText(project.content / "prefabs" / "stack.prefab",
                           R"({"version": 1, "entities": [], "nested": [{"id": 1, "source": "crate.prefab"}]})");
}

TEST_CASE("AssetImport: references") {
    TestProject project("asset-import-references");
    writeReferencingProject(project);
    EditorDiagnostics diagnostics;
    ProjectAssets assets(project.content, project.scenes, project.settingsFile(),
                         AssetCatalogOptions{.watchFiles = false});
    assets.refresh(diagnostics);

    const std::filesystem::path plate = normalizedCatalogPath(project.content / "textures" / "plate.png");
    const std::vector<AssetReferrer> plateUsers = assets.findReferences(plate, diagnostics);
    CHECK_MESSAGE((plateUsers.size() == 2 && plateUsers[0].kind == EditorAssetKind::Material &&
                   plateUsers[1].kind == EditorAssetKind::Scene),
                  "a texture should be found in a material and a scene");
    const std::vector<AssetReferrer> soundUsers =
        assets.findReferences(project.content / "sounds" / "hit.wav", diagnostics);
    CHECK_MESSAGE((soundUsers.size() == 1 && soundUsers.front().kind == EditorAssetKind::Scene),
                  "a legacy path string should count as a reference");
    const std::vector<AssetReferrer> crateUsers =
        assets.findReferences(project.content / "prefabs" / "crate.prefab", diagnostics);
    CHECK_MESSAGE((crateUsers.size() == 1 && crateUsers.front().file.filename() == "stack.prefab"),
                  "a nested prefab should be found in the prefab containing it");
    CHECK_MESSAGE(assets.findReferences(project.content / "textures" / "unused.png", diagnostics).empty(),
                  "an unused asset should have no references");
    CHECK_MESSAGE(assets.findReferences(project.content / "textures", diagnostics).size() == 2,
                  "a folder is used by whatever uses its contents");
    CHECK_MESSAGE(assets.findReferences(project.content / "prefabs", diagnostics).empty(),
                  "references within a folder should not keep it in use");

    // A GUID finds a reference whose path is stale.
    const AssetGuid guid = assets.catalog().findAsset(plate)->guid;
    testsupport::writeText(project.content / "materials" / "renamed.zmat",
                           R"({"version": 1, "base_color_texture": {"guid": ")" + guid.toString() +
                               R"(", "path": "../textures/old.png"}})");
    assets.refresh(diagnostics);
    CHECK_MESSAGE(assets.findReferences(plate, diagnostics).size() == 3, "a reference by GUID should be found");

    const std::string scene = testsupport::readText(project.scenes / "level.zenith-scene");
    const std::vector<SceneEntityId> users = sceneEntitiesReferencing(scene, project.content, plate, guid);
    CHECK_MESSAGE((users.size() == 1 && users.front().value == 7), "the open scene's users should be found by entity");
}

TEST_CASE("AssetImport: delete guard and find references") {
    TestProject project("asset-import-guard");
    writeReferencingProject(project);
    HeadlessImGui imgui;
    EditorDiagnostics diagnostics;
    ProjectAssets assets(project.content, project.scenes, project.settingsFile(),
                         AssetCatalogOptions{.watchFiles = false});
    assets.refresh(diagnostics);
    AssetBrowserPanel panel(assets);
    std::vector<SceneEntityId> selected;
    const AssetBrowserActions actions{
        .sceneUsers = [](const EditorAssetRecord&) { return std::vector<SceneEntityId>{SceneEntityId{7}}; },
        .selectEntities =
            [&selected](std::span<const SceneEntityId> entities) { selected.assign(entities.begin(), entities.end()); },
    };

    panel.requestDelete(project.content / "textures" / "plate.png", diagnostics);
    CHECK_MESSAGE(panel.deleteReferrers().size() == 2, "deleting a used asset should name its users");
    imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
    CHECK_MESSAGE(std::filesystem::exists(project.content / "textures" / "plate.png"),
                  "asking to delete should not delete before confirmation");
    panel.requestDelete(project.content / "textures" / "unused.png", diagnostics);
    CHECK_MESSAGE(panel.deleteReferrers().empty(), "deleting an unused asset should need no warning");

    panel.findReferences(project.content / "textures" / "plate.png", actions, diagnostics);
    CHECK_MESSAGE((panel.references().files.size() == 2 && panel.references().sceneEntities.size() == 1),
                  "Find References should list files and open-scene entities");
    imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
}

TEST_CASE("AssetImport: file drops") {
    TestProject project("asset-import-drops");
    const std::filesystem::path outside = project.directory / "outside";
    testsupport::writeText(outside / "crate.gltf", "{}");
    testsupport::writeText(outside / "far.png", "png");
    testsupport::writeText(project.content / "models" / "keep.txt", "folder content");
    HeadlessImGui imgui;
    EditorDiagnostics diagnostics;
    ProjectAssets assets(project.content, project.scenes, project.settingsFile(),
                         AssetCatalogOptions{.watchFiles = false});
    assets.refresh(diagnostics);
    AssetBrowserPanel panel(assets);
    FileDropQueue drops;
    const AssetBrowserActions actions{.fileDrops = &drops};

    const std::array<DroppedFile, 2> dropped{DroppedFile{.path = outside / "crate.gltf", .position = {400.0f, 300.0f}},
                                             DroppedFile{.path = outside / "far.png", .position = {1000.0f, 700.0f}}};
    drops.add(dropped);
    imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
    CHECK_MESSAGE(std::filesystem::exists(project.content / "crate.gltf"),
                  "a file dropped on the browser should be imported into its current folder");
    CHECK_MESSAGE(panel.selectedPath() == normalizedCatalogPath(project.content / "crate.gltf"),
                  "the imported file should be selected");
    CHECK_MESSAGE((drops.pending().size() == 1 && drops.pending().front().path.filename() == "far.png"),
                  "a drop outside the browser should be left for another window");
    CHECK_MESSAGE(!std::filesystem::exists(project.content / "far.png"), "a drop outside should not be imported");

    // A window over the browser takes the drops on it: the browser leaves a
    // drop inside its rectangle that landed on the window above it.
    drops.takeAll();
    testsupport::writeText(outside / "covered.gltf", "{}");
    const auto drawCovered = [&] {
        panel.drawBrowser(diagnostics, actions);
        ImGui::SetNextWindowPos({350.0f, 250.0f});
        ImGui::SetNextWindowSize({100.0f, 100.0f});
        ImGui::Begin("Cover");
        ImGui::End();
    };
    imgui.frame(drawCovered);
    const std::array<DroppedFile, 1> covered{
        DroppedFile{.path = outside / "covered.gltf", .position = {400.0f, 300.0f}}};
    drops.add(covered);
    imgui.frame(drawCovered);
    CHECK_MESSAGE((drops.pending().size() == 1 && !std::filesystem::exists(project.content / "covered.gltf")),
                  "a drop on a window above the browser should be left for that window");
}

struct EnemyDef {
    std::string id;
    int health{0};
};

TEST_CASE("AssetImport: data tables") {
    TestProject project("asset-import-data");
    const std::filesystem::path enemies = project.content / "data" / "enemies.json";
    testsupport::writeText(enemies,
                           "\xEF\xBB\xBF  [{\"id\": \"rat\", \"health\": 3}, {\"id\": \"bat\", \"speed\": 2}]");
    testsupport::writeText(project.content / "data" / "settings.json", R"({"volume": 1})");
    testsupport::writeText(project.content / "data" / "broken.json", R"([{"id": "a"}, {"id": "a"}])");
    CHECK_MESSAGE(editorAssetKind(enemies) == EditorAssetKind::Data, "a JSON array should be a data table");
    CHECK_MESSAGE(!editorAssetKind(project.content / "data" / "settings.json"),
                  "a JSON object should not be listed as an asset");

    EditorDiagnostics diagnostics;
    ProjectAssets assets(project.content, project.scenes, project.settingsFile(),
                         AssetCatalogOptions{.watchFiles = false});
    assets.refresh(diagnostics);
    CHECK_MESSAGE(
        (assets.catalog().findAsset(normalizedCatalogPath(enemies)) != nullptr &&
         assets.catalog().findAsset(normalizedCatalogPath(project.content / "data" / "settings.json")) == nullptr),
        "the catalog should list only data tables");

    DataTableView view;
    view.update(enemies, nullptr, nlohmann::json::object(), 0, diagnostics);
    CHECK_MESSAGE((view.error().empty() && view.schemaName().empty()),
                  "a well-formed table should pass the structure check");
    CHECK_MESSAGE((view.table().columns == std::vector<std::string>{"id", "health", "speed"} &&
                   view.table().rows.size() == 2 && view.table().rows[1][0] == "bat" &&
                   view.table().rows[1][1].empty()),
                  "columns should follow the keys in first-seen order");

    view.update(project.content / "data" / "broken.json", nullptr, nlohmann::json::object(), 0, diagnostics);
    CHECK_MESSAGE((view.error().find("duplicate id") != std::string::npos &&
                   hasMessage(diagnostics, "broken.json[1]", EditorDiagnosticSeverity::Warning)),
                  "a duplicate id should be reported to the log");

    DefinitionSchemaRegistry schemas;
    CHECK_MESSAGE((static_cast<bool>(schemas.add(makeDefinitionSchema<EnemyDef>(
                      "enemies", Fields<EnemyDef>().add("id", &EnemyDef::id).add("health", &EnemyDef::health))))),
                  "the schema should register");
    view.update(enemies, &schemas, nlohmann::json::object(), 0, diagnostics);
    CHECK_MESSAGE((view.schemaName() == "enemies" && view.error().find("unknown field 'speed'") != std::string::npos),
                  "a table named like a schema should be validated against it");
    const size_t warnings = diagnostics.entries().size();
    view.update(enemies, &schemas, nlohmann::json::object(), 0, diagnostics);
    CHECK_MESSAGE(diagnostics.entries().size() == warnings, "an unchanged table should not be reported again");
    view.update(enemies, &schemas, {{"schema", "items"}}, 0, diagnostics);
    CHECK_MESSAGE(view.schemaName().empty(),
                  "a schema named in the settings that is not registered checks structure only");
    CHECK_MESSAGE(definitionSchemaFor(&schemas, {{"schema", "enemies"}}, project.content / "x.json") != nullptr,
                  "the settings should name the schema");
}
} // namespace

TEST_SUITE_END();
