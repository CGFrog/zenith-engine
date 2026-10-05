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

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

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
    std::filesystem::path directory;
    std::filesystem::path content;
    std::filesystem::path scenes;

    explicit TestProject(std::string_view name)
        : directory(std::filesystem::temp_directory_path() / name)
        , content(directory / "content")
        , scenes(directory / "scenes") {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        std::filesystem::create_directories(content);
        std::filesystem::create_directories(scenes);
    }

    ~TestProject() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    TestProject(const TestProject&) = delete;
    TestProject& operator=(const TestProject&) = delete;

    std::filesystem::path settingsFile() const {
        return directory / "settings" / "asset-browser.json";
    }
};

void testImportIntoFolder() {
    TestProject project("zenith_asset_import_copy");
    const std::filesystem::path outside = project.directory / "outside";
    writeFile(outside / "stone.png", "stone");
    writeFile(outside / "stone.png.meta", "{}");
    writeFile(outside / "pack" / "a.wav", "a");
    writeFile(outside / "pack" / "a.wav.meta", "{}");
    writeFile(outside / "pack" / ".hidden", "h");
    writeFile(project.content / "stone.png", "existing");

    const std::array<std::filesystem::path, 3> sources{outside / "stone.png", outside / "pack",
                                                       outside / "stone.png.meta"};
    const AssetImportResult result = importIntoFolder(sources, project.content);
    expect(result.success() && result.imported.size() == 2, "a file and a folder should be imported");
    expect(std::filesystem::exists(project.content / "stone (1).png") &&
               readFile(project.content / "stone.png") == "existing",
           "a taken name should get ' (1)' and leave the existing file alone");
    expect(std::filesystem::exists(project.content / "pack" / "a.wav") &&
               !std::filesystem::exists(project.content / "pack" / "a.wav.meta") &&
               !std::filesystem::exists(project.content / "pack" / ".hidden"),
           "a folder's sidecars and hidden files should stay behind");
    expect(uniqueImportPath(project.content, "stone.png").filename() == "stone (2).png",
           "the next copy should count on");
    expect(uniqueImportPath(project.content, "pack").filename() == "pack (1)", "folders have no extension");

    const std::array<std::filesystem::path, 2> invalid{outside / "missing.png", project.directory};
    const AssetImportResult failed = importIntoFolder(invalid, project.content);
    expect(failed.errors.size() == 2 && failed.imported.empty(),
           "a missing source and a folder containing the destination should be refused");
}

void testProjectImportAndSettings() {
    TestProject project("zenith_asset_import_project");
    const std::filesystem::path outside = project.directory / "outside";
    writeFile(outside / "wall.png", "png");
    writeFile(project.content / "sounds" / "hit.wav", "wav");
    writeFile(project.scenes / "level.zenith-scene", "{}");

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
    expect(imported.size() == 1 && imported.front() == wall && record != nullptr && record->guid,
           "an imported file should be listed with a GUID");
    expect(std::filesystem::exists(assetMetaPath(wall)), "an imported file should get a .meta sidecar");
    expect(hasMessage(diagnostics, "Imported 'wall.png'", EditorDiagnosticSeverity::Info),
           "the import should be reported");
    expect(assets.importFiles(sources, project.directory, diagnostics).empty() &&
               hasMessage(diagnostics, "Cannot import into", EditorDiagnosticSeverity::Error),
           "importing outside the project's folders should be refused");

    expect(assets.importSettings(wall) == nlohmann::json::object(), "a new asset should have empty settings");
    expect(assets.setImportSettings(wall, {{"usage", "normal"}}, diagnostics), "settings should be stored");
    expect(assets.importSettings(wall) == nlohmann::json{{"usage", "normal"}}, "stored settings should read back");
    AssetMeta meta;
    expect(readAssetMeta(assetMetaPath(wall), meta) && meta.importSettings["usage"] == "normal",
           "the settings should be written to the sidecar");
    assets.pollChanges(diagnostics, actions);
    expect(reloadedTextures.size() == 1 && reloadedTextures.front() == wall,
           "applying settings should reimport the texture");

    const std::filesystem::path hit = normalizedCatalogPath(project.content / "sounds" / "hit.wav");
    expect(assets.queueReimport(hit), "a sound should be reimportable");
    expect(!assets.queueReimport(normalizedCatalogPath(project.scenes / "level.zenith-scene")),
           "a scene has nothing to reimport");
    assets.pollChanges(diagnostics, actions);
    expect(reloadedSounds.size() == 1 && reloadedSounds.front() == hit, "Reimport should reload the sound");
}

void writeReferencingProject(const TestProject& project) {
    writeFile(project.content / "textures" / "plate.png", "png");
    writeFile(project.content / "textures" / "unused.png", "png");
    writeFile(project.content / "sounds" / "hit.wav", "wav");
    writeFile(project.content / "materials" / "plate.zmat",
              R"({"version": 1, "base_color_texture": {"path": "../textures/plate.png"}})");
    writeFile(project.scenes / "level.zenith-scene", R"({"version": 2, "entities": [
        {"id": 7, "components": {"decal": {"albedo_asset": {"path": "textures/plate.png"}}}},
        {"id": 8, "components": {"audio_emitter": {"clip_asset": "sounds/hit.wav"}}}]})");
    writeFile(project.content / "prefabs" / "crate.prefab", R"({"version": 1, "entities": [], "nested": []})");
    writeFile(project.content / "prefabs" / "stack.prefab",
              R"({"version": 1, "entities": [], "nested": [{"id": 1, "source": "crate.prefab"}]})");
}

void testReferences() {
    TestProject project("zenith_asset_import_references");
    writeReferencingProject(project);
    EditorDiagnostics diagnostics;
    ProjectAssets assets(project.content, project.scenes, project.settingsFile(),
                         AssetCatalogOptions{.watchFiles = false});
    assets.refresh(diagnostics);

    const std::filesystem::path plate = normalizedCatalogPath(project.content / "textures" / "plate.png");
    const std::vector<AssetReferrer> plateUsers = assets.findReferences(plate, diagnostics);
    expect(plateUsers.size() == 2 && plateUsers[0].kind == EditorAssetKind::Material &&
               plateUsers[1].kind == EditorAssetKind::Scene,
           "a texture should be found in a material and a scene");
    const std::vector<AssetReferrer> soundUsers =
        assets.findReferences(project.content / "sounds" / "hit.wav", diagnostics);
    expect(soundUsers.size() == 1 && soundUsers.front().kind == EditorAssetKind::Scene,
           "a legacy path string should count as a reference");
    const std::vector<AssetReferrer> crateUsers =
        assets.findReferences(project.content / "prefabs" / "crate.prefab", diagnostics);
    expect(crateUsers.size() == 1 && crateUsers.front().file.filename() == "stack.prefab",
           "a nested prefab should be found in the prefab containing it");
    expect(assets.findReferences(project.content / "textures" / "unused.png", diagnostics).empty(),
           "an unused asset should have no references");
    expect(assets.findReferences(project.content / "textures", diagnostics).size() == 2,
           "a folder is used by whatever uses its contents");
    expect(assets.findReferences(project.content / "prefabs", diagnostics).empty(),
           "references within a folder should not keep it in use");

    // A GUID finds a reference whose path is stale.
    const AssetGuid guid = assets.catalog().findAsset(plate)->guid;
    writeFile(project.content / "materials" / "renamed.zmat", R"({"version": 1, "base_color_texture": {"guid": ")" +
                                                                  guid.toString() +
                                                                  R"(", "path": "../textures/old.png"}})");
    assets.refresh(diagnostics);
    expect(assets.findReferences(plate, diagnostics).size() == 3, "a reference by GUID should be found");

    const std::string scene = readFile(project.scenes / "level.zenith-scene");
    const std::vector<SceneEntityId> users = sceneEntitiesReferencing(scene, project.content, plate, guid);
    expect(users.size() == 1 && users.front().value == 7, "the open scene's users should be found by entity");
}

void testDeleteGuardAndFindReferences() {
    TestProject project("zenith_asset_import_guard");
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
    expect(panel.deleteReferrers().size() == 2, "deleting a used asset should name its users");
    imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
    expect(std::filesystem::exists(project.content / "textures" / "plate.png"),
           "asking to delete should not delete before confirmation");
    panel.requestDelete(project.content / "textures" / "unused.png", diagnostics);
    expect(panel.deleteReferrers().empty(), "deleting an unused asset should need no warning");

    panel.findReferences(project.content / "textures" / "plate.png", actions, diagnostics);
    expect(panel.references().files.size() == 2 && panel.references().sceneEntities.size() == 1,
           "Find References should list files and open-scene entities");
    imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
}

void testFileDrops() {
    TestProject project("zenith_asset_import_drops");
    const std::filesystem::path outside = project.directory / "outside";
    writeFile(outside / "crate.gltf", "{}");
    writeFile(outside / "far.png", "png");
    writeFile(project.content / "models" / "keep.txt", "folder content");
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
    expect(std::filesystem::exists(project.content / "crate.gltf"),
           "a file dropped on the browser should be imported into its current folder");
    expect(panel.selectedPath() == normalizedCatalogPath(project.content / "crate.gltf"),
           "the imported file should be selected");
    expect(drops.pending().size() == 1 && drops.pending().front().path.filename() == "far.png",
           "a drop outside the browser should be left for another window");
    expect(!std::filesystem::exists(project.content / "far.png"), "a drop outside should not be imported");
}

struct EnemyDef {
    std::string id;
    int health{0};
};

void testDataTables() {
    TestProject project("zenith_asset_import_data");
    const std::filesystem::path enemies = project.content / "data" / "enemies.json";
    writeFile(enemies, "\xEF\xBB\xBF  [{\"id\": \"rat\", \"health\": 3}, {\"id\": \"bat\", \"speed\": 2}]");
    writeFile(project.content / "data" / "settings.json", R"({"volume": 1})");
    writeFile(project.content / "data" / "broken.json", R"([{"id": "a"}, {"id": "a"}])");
    expect(editorAssetKind(enemies) == EditorAssetKind::Data, "a JSON array should be a data table");
    expect(!editorAssetKind(project.content / "data" / "settings.json"),
           "a JSON object should not be listed as an asset");

    EditorDiagnostics diagnostics;
    ProjectAssets assets(project.content, project.scenes, project.settingsFile(),
                         AssetCatalogOptions{.watchFiles = false});
    assets.refresh(diagnostics);
    expect(assets.catalog().findAsset(normalizedCatalogPath(enemies)) != nullptr &&
               assets.catalog().findAsset(normalizedCatalogPath(project.content / "data" / "settings.json")) == nullptr,
           "the catalog should list only data tables");

    DataTableView view;
    view.update(enemies, nullptr, nlohmann::json::object(), 0, diagnostics);
    expect(view.error().empty() && view.schemaName().empty(), "a well-formed table should pass the structure check");
    expect(view.table().columns == std::vector<std::string>{"id", "health", "speed"} && view.table().rows.size() == 2 &&
               view.table().rows[1][0] == "bat" && view.table().rows[1][1].empty(),
           "columns should follow the keys in first-seen order");

    view.update(project.content / "data" / "broken.json", nullptr, nlohmann::json::object(), 0, diagnostics);
    expect(view.error().find("duplicate id") != std::string::npos &&
               hasMessage(diagnostics, "broken.json[1]", EditorDiagnosticSeverity::Warning),
           "a duplicate id should be reported to the log");

    DefinitionSchemaRegistry schemas;
    expect(static_cast<bool>(schemas.add(makeDefinitionSchema<EnemyDef>(
               "enemies", Fields<EnemyDef>().add("id", &EnemyDef::id).add("health", &EnemyDef::health)))),
           "the schema should register");
    view.update(enemies, &schemas, nlohmann::json::object(), 0, diagnostics);
    expect(view.schemaName() == "enemies" && view.error().find("unknown field 'speed'") != std::string::npos,
           "a table named like a schema should be validated against it");
    const size_t warnings = diagnostics.entries().size();
    view.update(enemies, &schemas, nlohmann::json::object(), 0, diagnostics);
    expect(diagnostics.entries().size() == warnings, "an unchanged table should not be reported again");
    view.update(enemies, &schemas, {{"schema", "items"}}, 0, diagnostics);
    expect(view.schemaName().empty(), "a schema named in the settings that is not registered checks structure only");
    expect(definitionSchemaFor(&schemas, {{"schema", "enemies"}}, project.content / "x.json") != nullptr,
           "the settings should name the schema");
}
} // namespace

int main() {
    testImportIntoFolder();
    testProjectImportAndSettings();
    testReferences();
    testDeleteGuardAndFindReferences();
    testFileDrops();
    testDataTables();
    if (failures != 0) {
        std::cerr << failures << " asset import check(s) failed\n";
        return 1;
    }
    std::cout << "Asset import tests passed\n";
    return 0;
}
