#include "editor/application/EditorDiagnostics.hpp"
#include "editor/panels/AssetBrowserPanel.hpp"
#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/settings/AssetBrowserSettings.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

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

bool pathExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool hasFolder(const AssetCatalog& catalog, const std::filesystem::path& path) {
    return catalog.findFolder(normalizedCatalogPath(path)) != nullptr;
}

bool hasErrorContaining(const EditorDiagnostics& diagnostics, std::string_view text,
                        EditorDiagnosticSeverity severity = EditorDiagnosticSeverity::Error) {
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
        ImGui::Begin("Asset Browser");
        draw();
        ImGui::End();
        ImGui::Render();
    }
};

void testCatalogFolders(const std::filesystem::path& content, const std::filesystem::path& scenes) {
    AssetCatalog catalog(content, scenes);
    expect(static_cast<bool>(catalog.refresh()), "the catalog should scan both roots");
    expect(catalog.roots().size() == 2 && catalog.roots().front() == normalizedCatalogPath(content),
           "catalog roots should be the normalized asset root then the scene root");
    expect(hasFolder(catalog, content) && hasFolder(catalog, scenes), "roots should be listed as folders");
    expect(hasFolder(catalog, content / "materials" / "empty"), "empty folders should be listed");
    const auto rootChildren = catalog.subfolders(normalizedCatalogPath(content));
    expect(rootChildren.size() == 1 && rootChildren.front()->absolutePath.filename() == "materials",
           "subfolders should list direct children only");
    expect(catalog.assetsInFolder(normalizedCatalogPath(content)).size() == 1 &&
               catalog.assetsInFolder(normalizedCatalogPath(content / "materials")).size() == 1,
           "assetsInFolder should list only the folder's own assets");
    const EditorAssetFolder* materials = catalog.findFolder(normalizedCatalogPath(content / "materials"));
    expect(materials != nullptr && materials->relativePath == std::filesystem::path("materials"),
           "folders should carry a path relative to their root");
}

void testNameValidation() {
    expect(invalidFileNameReason("Level 1").empty(), "ordinary names should be valid");
    expect(!invalidFileNameReason("").empty(), "empty names should be rejected");
    expect(!invalidFileNameReason("a/b").empty() && !invalidFileNameReason("a\\b").empty(),
           "names with separators should be rejected");
    expect(!invalidFileNameReason("..").empty(), "parent references should be rejected");
    expect(!invalidFileNameReason("name.").empty(), "trailing periods should be rejected");
    expect(!invalidFileNameReason("CON.png").empty(), "Windows device names should be rejected");
}

void testFileOperations(const std::filesystem::path& root) {
    const std::filesystem::path folder = root / "operations";
    std::filesystem::create_directories(folder);

    const ProjectFileResult firstFolder = createProjectFolder(folder);
    const ProjectFileResult secondFolder = createProjectFolder(folder);
    expect(firstFolder && firstFolder.path.filename() == "New Folder" &&
               std::filesystem::is_directory(firstFolder.path),
           "creating a folder should make 'New Folder'");
    expect(secondFolder && secondFolder.path.filename() == "New Folder 2",
           "creating another folder should pick a unique name");

    const ProjectFileResult firstScene = createProjectScene(folder, root);
    const ProjectFileResult secondScene = createProjectScene(folder, root);
    expect(firstScene && firstScene.path.filename() == "New Scene.zenith-scene" &&
               secondScene.path.filename() == "New Scene 2.zenith-scene",
           "created scenes should get unique names");
    Scene loaded;
    AssetManager assets;
    expect(firstScene && SceneDocument::load(firstScene.path, assets, {.assetRoot = root}, loaded),
           "a created scene should be a valid empty scene document");

    writeFile(folder / "albedo.png", "albedo");
    writeFile(folder / "other.png", "other");
    const ProjectFileResult renamed = renameProjectItem(folder / "albedo.png", "base");
    expect(renamed && renamed.path.filename() == "base.png" && readFile(folder / "base.png") == "albedo",
           "renaming a file should keep its extension");
    const ProjectFileResult collision = renameProjectItem(folder / "base.png", "other");
    expect(!collision && readFile(folder / "other.png") == "other" && readFile(folder / "base.png") == "albedo",
           "renaming must refuse to overwrite an existing file");
    expect(renameProjectItem(folder / "base.png", "base2.PNG").path.filename() == "base2.PNG",
           "a name that already ends with the extension should not repeat it");
    expect(!renameProjectItem(folder / "base2.PNG", "bad:name"), "renaming should reject invalid names");
    expect(!renameProjectItem(folder / "missing.png", "anything"), "renaming a missing file should fail");
    const ProjectFileResult folderRename = renameProjectItem(firstFolder.path, "Textures.v2");
    expect(folderRename && folderRename.path.filename() == "Textures.v2" &&
               std::filesystem::is_directory(folderRename.path),
           "renaming a folder should use the name as given");

    const ProjectFileResult copy = duplicateProjectItem(folder / "other.png");
    const ProjectFileResult secondCopy = duplicateProjectItem(folder / "other.png");
    expect(copy && copy.path.filename() == "other copy.png" && readFile(copy.path) == "other",
           "duplicating a file should copy it beside the original");
    expect(secondCopy && secondCopy.path.filename() == "other copy 2.png",
           "a second duplicate should get a unique name");
    writeFile(folderRename.path / "inner.txt", "inner");
    const ProjectFileResult folderCopy = duplicateProjectItem(folderRename.path);
    expect(folderCopy && folderCopy.path.filename() == "Textures.v2 copy" &&
               readFile(folderCopy.path / "inner.txt") == "inner",
           "duplicating a folder should copy its contents");

    expect(!moveProjectItemToTrash(folder / "does-not-exist.png"), "trashing a missing item should fail");
    expect(!revealInFileBrowser(folder / "does-not-exist.png"), "revealing a missing item should fail");

#if defined(__linux__)
    // A private XDG data home keeps the test out of the user's real trash.
    const std::filesystem::path dataHome = root / "xdg-data";
    setenv("XDG_DATA_HOME", dataHome.c_str(), 1);
    const ProjectFileResult trashed = moveProjectItemToTrash(folder / "other copy.png");
    expect(trashed && !pathExists(folder / "other copy.png") &&
               pathExists(dataHome / "Trash" / "files" / "other copy.png") &&
               readFile(dataHome / "Trash" / "info" / "other copy.png.trashinfo").find("Path=/") != std::string::npos,
           "trashing should move the file into the XDG trash with its trash info");
    writeFile(folder / "other copy.png", "again");
    const ProjectFileResult trashedAgain = moveProjectItemToTrash(folder / "other copy.png");
    expect(trashedAgain && pathExists(dataHome / "Trash" / "files" / "other copy.2.png"),
           "trashing a second item with the same name should not replace the first");
#elif defined(_WIN32)
    // The Recycle Bin is the user's real one, so this runs only on request.
    char* requested = nullptr;
    size_t requestedLength = 0;
    const bool trashRequested =
        _dupenv_s(&requested, &requestedLength, "ZENITH_TEST_OS_TRASH") == 0 && requested != nullptr;
    std::free(requested);
    if (trashRequested) {
        const ProjectFileResult trashed = moveProjectItemToTrash(folder / "other copy.png");
        expect(trashed && !pathExists(folder / "other copy.png"), "trashing should move the file to the Recycle Bin");
    }
#endif
}

void testSettings(const std::filesystem::path& root) {
    const std::filesystem::path path = root / "settings" / "asset-browser.json";
    AssetBrowserSettings settings;
    settings.favourites = {"stale"};
    expect(loadAssetBrowserSettings(path, settings) && settings == AssetBrowserSettings{},
           "a missing settings file should load the defaults");

    const AssetBrowserSettings saved{
        .viewMode = AssetBrowserViewMode::List, .favourites = {"materials", "../scenes"}, .lastFolder = "materials"};
    expect(static_cast<bool>(saveAssetBrowserSettings(path, saved)), "settings should save");
    AssetBrowserSettings reloaded;
    expect(loadAssetBrowserSettings(path, reloaded) && reloaded == saved, "settings should round-trip");

    for (const std::string_view corrupt :
         {std::string_view("{not json"), std::string_view("[]"), std::string_view(R"({"version":1,"viewMode":3})"),
          std::string_view(R"({"version":1,"favourites":[1]})"), std::string_view(R"({"version":"1"})")}) {
        writeFile(path, corrupt);
        AssetBrowserSettings fallback = saved;
        const AssetBrowserSettingsResult result = loadAssetBrowserSettings(path, fallback);
        expect(!result && !result.error.empty() && fallback == AssetBrowserSettings{},
               "corrupt settings should report an error and fall back to the defaults");
    }
}

void drawAllModes(HeadlessImGui& imgui, AssetBrowserPanel& panel, EditorDiagnostics& diagnostics,
                  const AssetBrowserActions& actions) {
    for (const AssetBrowserViewMode mode : {AssetBrowserViewMode::Grid, AssetBrowserViewMode::List}) {
        panel.setViewMode(mode, diagnostics);
        for (const char* search : {"", "a", "no-such-asset"}) {
            panel.setSearchText(search);
            // Two frames so tables and children settle their layout.
            imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
            imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
        }
    }
    panel.setSearchText({});
    panel.setKindShown(EditorAssetKind::Texture, false);
    imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
    panel.setKindShown(EditorAssetKind::Texture, true);
}

void testPanel(const std::filesystem::path& content, const std::filesystem::path& scenes,
               const std::filesystem::path& settingsDirectory) {
    const std::filesystem::path settingsFile = settingsDirectory / "asset-browser.json";
    const std::filesystem::path materials = normalizedCatalogPath(content / "materials");
    const std::filesystem::path surfaces = normalizedCatalogPath(content / "surfaces");
    HeadlessImGui imgui;
    int openedScenes = 0;
    const AssetBrowserActions actions{.openScene = [&openedScenes](const std::filesystem::path&) { ++openedScenes; },
                                      .reloadModel = {}};
    {
        EditorDiagnostics diagnostics;
        AssetBrowserPanel panel(content, scenes, settingsFile);
        panel.refresh(diagnostics);
        expect(panel.currentFolder() == normalizedCatalogPath(content), "the browser should start at the asset root");
        imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });

        panel.navigateTo(materials, diagnostics);
        expect(panel.currentFolder() == materials, "navigating should change the current folder");
        panel.navigateTo(content / "not-a-folder", diagnostics);
        expect(panel.currentFolder() == materials && hasErrorContaining(diagnostics, "not a project folder"),
               "navigating to an unknown folder should be refused");
        panel.toggleFavourite(materials, diagnostics);
        panel.toggleFavourite(scenes, diagnostics);
        expect(panel.isFavourite(materials) && panel.isFavourite(scenes), "folders should become favourites");
        drawAllModes(imgui, panel, diagnostics, actions);

        panel.createScene(diagnostics);
        const std::filesystem::path createdScene = panel.selectedPath();
        expect(createdScene.filename() == "New Scene.zenith-scene" && panel.catalog().findAsset(createdScene),
               "creating a scene should add it to the current folder and select it");
        panel.duplicateItem(createdScene, diagnostics);
        expect(panel.selectedPath().filename() == "New Scene copy.zenith-scene", "duplicating should select the copy");
        panel.createFolder(diagnostics);
        expect(panel.selectedPath().filename() == "New Folder" && panel.catalog().findFolder(panel.selectedPath()),
               "creating a folder should add and select it");
        drawAllModes(imgui, panel, diagnostics, actions);

        panel.renameItem(materials, "surfaces", diagnostics);
        expect(panel.currentFolder() == surfaces && panel.isFavourite(surfaces) && !panel.isFavourite(materials),
               "renaming a folder should carry the current folder and favourites along");
        panel.renameItem(normalizedCatalogPath(content), "renamed", diagnostics);
        expect(hasErrorContaining(diagnostics, "cannot be renamed") && pathExists(content),
               "project roots should not be renamable");
        panel.renameItem(surfaces / "New Scene.zenith-scene", "New Scene copy", diagnostics);
        expect(pathExists(surfaces / "New Scene.zenith-scene") && hasErrorContaining(diagnostics, "already exists"),
               "renaming onto an existing asset should be refused and reported");
        panel.setViewMode(AssetBrowserViewMode::List, diagnostics);
        imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
        expect(openedScenes == 0, "drawing without input should not open scenes");
    }
    {
        // A fresh panel restores the view mode, favourites, and last folder.
        EditorDiagnostics diagnostics;
        AssetBrowserPanel panel(content, scenes, settingsFile);
        panel.refresh(diagnostics);
        expect(panel.viewMode() == AssetBrowserViewMode::List, "the view mode should persist");
        expect(panel.isFavourite(surfaces) && panel.isFavourite(scenes), "favourites should persist");
        expect(panel.currentFolder() == surfaces, "the last folder should persist");
        imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
        expect(diagnostics.entries().empty(), "restoring valid settings should report nothing");
    }
    {
        // Corrupt settings fall back to the defaults with a warning.
        writeFile(settingsFile, "{ corrupt");
        EditorDiagnostics diagnostics;
        AssetBrowserPanel panel(content, scenes, settingsFile);
        panel.refresh(diagnostics);
        imgui.frame([&] { panel.drawBrowser(diagnostics, actions); });
        expect(panel.viewMode() == AssetBrowserViewMode::Grid && !panel.isFavourite(surfaces) &&
                   panel.currentFolder() == normalizedCatalogPath(content),
               "corrupt settings should fall back to the defaults");
        expect(hasErrorContaining(diagnostics, "Invalid asset browser settings", EditorDiagnosticSeverity::Warning),
               "corrupt settings should be reported");
    }
    {
        // A last folder that has since been removed falls back to a parent.
        AssetBrowserSettings settings{.lastFolder = "surfaces/gone/deeper"};
        saveAssetBrowserSettings(settingsFile, settings);
        EditorDiagnostics diagnostics;
        AssetBrowserPanel panel(content, scenes, settingsFile);
        panel.refresh(diagnostics);
        expect(panel.currentFolder() == surfaces, "a missing last folder should fall back to its nearest ancestor");
    }
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_asset_browser_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    const std::filesystem::path content = directory / "content";
    const std::filesystem::path scenes = directory / "scenes";
    std::filesystem::create_directories(content / "materials" / "empty");
    std::filesystem::create_directories(scenes);
    writeFile(content / "model.gltf", "{}");
    writeFile(content / "materials" / "albedo.png", "png");
    writeFile(scenes / "room.zenith-scene", "{}");

    testCatalogFolders(content, scenes);
    testNameValidation();
    testFileOperations(directory);
    testSettings(directory);
    testPanel(content, scenes, directory / "settings");

    std::filesystem::remove_all(directory, error);
    if (failures != 0) {
        std::cerr << failures << " asset browser test(s) failed\n";
        return 1;
    }
    return 0;
}
