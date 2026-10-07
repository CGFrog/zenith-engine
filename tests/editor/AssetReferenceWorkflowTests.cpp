// End-to-end coverage of GUID asset references in the editor: scenes save
// version 2 references through the browser's asset database, survive renames
// and moves made in the browser and behind its back, the open document
// follows its own file, and changed models reload automatically.

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/panels/AssetBrowserPanel.hpp"
#include "editor/project/AssetRelocation.hpp"
#include "engine/assets/AssetMeta.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_BEGIN("AssetReferenceWorkflow");

namespace {

nlohmann::json readJson(const std::filesystem::path& path) {
    return nlohmann::json::parse(testsupport::readText(path), nullptr, false);
}

AssetGuid sidecarGuid(const std::filesystem::path& asset) {
    AssetMeta meta;
    return readAssetMeta(assetMetaPath(asset), meta) ? meta.guid : AssetGuid{};
}

const nlohmann::json& componentOf(const nlohmann::json& document, std::string_view component) {
    static const nlohmann::json empty = nlohmann::json::object();
    for (const nlohmann::json& entity : document.at("entities")) {
        if (entity.at("components").contains(component)) {
            return entity.at("components").at(component);
        }
    }
    return empty;
}

// What the editor session does with the browser's notifications, recorded.
struct EditorStandIn {
    EditorDiagnostics diagnostics;
    AssetManager assets;
    Scene scene;
    EditorDocument document;
    std::vector<ProjectFileMove> moves;
    std::vector<std::filesystem::path> removed;
    std::vector<std::filesystem::path> reloadedModels;
    bool lastReloadSucceeded{false};

    explicit EditorStandIn(const std::filesystem::path& assetRoot)
        : document(assetRoot) {}

    ProjectAssetActions actions() {
        return {
            .reloadModel =
                [this](const std::filesystem::path& path) {
                    reloadedModels.push_back(path);
                    lastReloadSucceeded = static_cast<bool>(assets.reloadModel(path));
                },
            .filesMoved =
                [this](std::span<const ProjectFileMove> moved) {
                    moves.insert(moves.end(), moved.begin(), moved.end());
                    if (document.path()) {
                        if (auto relocated = relocatedProjectPath(normalizedCatalogPath(*document.path()), moved)) {
                            document.relocate(*relocated);
                        }
                    }
                    size_t count = 0;
                    const SceneDocumentResult result =
                        relocateSceneAssets(scene, assets, document.options(), moved, count);
                    CHECK_MESSAGE(result.success, ("relocating the open scene should succeed: " + result.error));
                },
            .filesRemoved =
                [this](std::span<const std::filesystem::path> gone) {
                    removed.insert(removed.end(), gone.begin(), gone.end());
                    if (document.path() && isRemovedProjectPath(normalizedCatalogPath(*document.path()), gone)) {
                        document.detach();
                    }
                },
        };
    }

    bool moved(const std::filesystem::path& to) const {
        const std::filesystem::path normalized = normalizedCatalogPath(to);
        return std::ranges::any_of(moves, [&normalized](const ProjectFileMove& move) { return move.to == normalized; });
    }
};

// Polls the project like the editor's frame loop until done() holds, for at
// most ten seconds.
bool waitFor(ProjectAssets& project, EditorStandIn& editor, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const ProjectAssetActions actions = editor.actions();
    while (std::chrono::steady_clock::now() < deadline) {
        project.pollChanges(editor.diagnostics, actions);
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

std::string serialized(EditorStandIn& editor) {
    std::string output;
    const SceneDocumentResult result =
        SceneDocument::serialize(editor.scene, editor.assets, editor.document.options(), output);
    CHECK_MESSAGE(result.success, result.error);
    return output;
}

TEST_CASE("AssetReferenceWorkflow: workflow") {
    const std::filesystem::path sourceAssets = ZENITH_ASSET_DIR;
    const testsupport::TempDirectory root("asset-reference-workflow");
    const std::filesystem::path& directory = root.path();
    const std::filesystem::path content = directory / "content";
    const std::filesystem::path scenes = directory / "scenes";
    std::filesystem::create_directories(content / "models");
    std::filesystem::create_directories(content / "textures");
    std::filesystem::create_directories(content / "props");
    std::filesystem::create_directories(scenes);
    std::filesystem::copy_file(sourceAssets / "primitives" / "cube.gltf", content / "models" / "cube.gltf");
    std::filesystem::copy_file(sourceAssets / "showcase" / "decals" / "decal_arrow.png",
                               content / "textures" / "arrow.png");

    AssetCatalogOptions catalogOptions;
    catalogOptions.watcher.debounce = std::chrono::milliseconds(50);
    catalogOptions.watcher.maxDelay = std::chrono::milliseconds(500);
    catalogOptions.watcher.pollingInterval = std::chrono::milliseconds(100);
    ProjectAssets project(content, scenes, directory / "settings" / "asset-browser.json", catalogOptions);
    AssetBrowserPanel panel(project);
    EditorStandIn editor(content);
    const ProjectAssetActions actions = editor.actions();
    // The first poll scans the project, the second starts the watchers.
    project.pollChanges(editor.diagnostics, actions);
    project.pollChanges(editor.diagnostics, actions);
    CHECK_MESSAGE(project.catalog().watching(), "the browser should watch the project");
    editor.document.setAssetDatabase(project.assetDatabase());
    const AssetGuid modelGuid = sidecarGuid(content / "models" / "cube.gltf");
    const AssetGuid textureGuid = sidecarGuid(content / "textures" / "arrow.png");
    CHECK_MESSAGE((modelGuid && textureGuid), "scanning should give every asset a sidecar GUID");

    // A scene referencing a model and a texture saves version 2 references.
    const AssetLoadResult<ModelHandle> model = editor.assets.loadModel(content / "models" / "cube.gltf");
    CHECK_MESSAGE(static_cast<bool>(model), ("the copied cube should load: " + model.error));
    const Entity cube = editor.scene.createEntity("Cube");
    CHECK_MESSAGE(editor.scene.instantiateModel(cube, model.handle, editor.assets).success,
                  "the cube should instantiate");
    const Entity decal = editor.scene.createEntity("Decal");
    editor.scene.registry().emplace<DecalComponent>(decal, DecalComponent{.albedoAsset = "textures/arrow.png"});
    const std::filesystem::path scenePath = normalizedCatalogPath(scenes / "level.zenith-scene");
    const SceneDocumentResult saved = editor.document.saveAs(scenePath, editor.scene, editor.assets);
    CHECK_MESSAGE(saved.success, ("the scene should save: " + saved.error));
    const nlohmann::json savedDocument = readJson(scenePath);
    CHECK_MESSAGE(savedDocument.value("version", 0u) == SceneDocument::currentVersion,
                  "the scene should save as the current version");
    CHECK_MESSAGE((componentOf(savedDocument, "model").value("guid", "") == modelGuid.toString() &&
                   componentOf(savedDocument, "decal").at("albedo_asset").value("guid", "") == textureGuid.toString()),
                  "saved references should carry the assets' GUIDs");

    // Renaming in the browser carries the sidecar and the open scene follows.
    panel.renameItem(content / "models" / "cube.gltf", "box", editor.diagnostics);
    project.dispatchChanges(editor.diagnostics, actions);
    CHECK_MESSAGE((sidecarGuid(content / "models" / "box.gltf") == modelGuid &&
                   !std::filesystem::exists(assetMetaPath(content / "models" / "cube.gltf"))),
                  "renaming in the browser should move the sidecar and keep the GUID");
    CHECK_MESSAGE(editor.moved(content / "models" / "box.gltf"), "a browser rename should be reported as a move");
    CHECK_MESSAGE(serialized(editor).find("models/box.gltf") != std::string::npos,
                  "the open scene's references should follow a renamed model");

    // Moving in the browser does the same.
    panel.moveItem(content / "models" / "box.gltf", content / "props", editor.diagnostics);
    project.dispatchChanges(editor.diagnostics, actions);
    CHECK_MESSAGE(sidecarGuid(content / "props" / "box.gltf") == modelGuid, "moving should keep the GUID");
    CHECK_MESSAGE(serialized(editor).find("props/box.gltf") != std::string::npos,
                  "the open scene's references should follow a moved model");

    // Behind the browser's back: the texture and its sidecar move on disk.
    std::filesystem::create_directories(content / "textures" / "signs");
    std::filesystem::rename(content / "textures" / "arrow.png", content / "textures" / "signs" / "arrow.png");
    std::filesystem::rename(assetMetaPath(content / "textures" / "arrow.png"),
                            assetMetaPath(content / "textures" / "signs" / "arrow.png"));
    CHECK_MESSAGE(waitFor(project, editor, [&] { return editor.moved(content / "textures" / "signs" / "arrow.png"); }),
                  "the watcher should report a texture moved outside the editor");
    CHECK_MESSAGE(project.catalog().findAsset(normalizedCatalogPath(content / "textures" / "signs" / "arrow.png")) !=
                      nullptr,
                  "the catalog should list the moved texture");

    // The file on disk still names the old paths; GUIDs find the assets.
    Scene reloaded;
    const SceneDocumentResult reloadResult =
        SceneDocument::load(scenePath, editor.assets, editor.document.options(), reloaded);
    CHECK_MESSAGE(reloadResult.success, ("a scene referencing moved assets should still load: " + reloadResult.error));
    const auto reloadedDecals = reloaded.registry().view<const DecalComponent>();
    CHECK_MESSAGE(
        (reloadedDecals.size() == 1 &&
         reloadedDecals.get<const DecalComponent>(reloadedDecals.front()).albedoAsset == "textures/signs/arrow.png"),
        "a moved texture reference should resolve to the new path");
    std::string reloadedText;
    SceneDocument::serialize(reloaded, editor.assets, editor.document.options(), reloadedText);
    CHECK_MESSAGE(reloadedText.find("props/box.gltf") != std::string::npos,
                  "a moved model reference should resolve to the new path");

    // Renaming the open scene in the browser moves the document with it.
    panel.renameItem(scenePath, "renamed", editor.diagnostics);
    project.dispatchChanges(editor.diagnostics, actions);
    const std::filesystem::path renamedScene = normalizedCatalogPath(scenes / "renamed.zenith-scene");
    CHECK_MESSAGE((editor.document.path() && normalizedCatalogPath(*editor.document.path()) == renamedScene),
                  "renaming the open scene should update the document path");
    CHECK_MESSAGE((editor.document.save(editor.scene, editor.assets).success && std::filesystem::exists(renamedScene) &&
                   !std::filesystem::exists(scenePath)),
                  "saving after the rename should write the renamed file");

    // A changed model reloads automatically.
    {
        std::ofstream append(content / "props" / "box.gltf", std::ios::binary | std::ios::app);
        append << "\n\n";
    }
    const std::filesystem::path boxPath = normalizedCatalogPath(content / "props" / "box.gltf");
    CHECK_MESSAGE(
        waitFor(project, editor,
                [&] { return std::ranges::find(editor.reloadedModels, boxPath) != editor.reloadedModels.end(); }),
        "a model changed on disk should be reloaded automatically");
    CHECK_MESSAGE(editor.lastReloadSucceeded, "the automatic model reload should succeed");

    // Deleting the open scene behind the browser's back detaches the document.
    std::filesystem::remove(renamedScene);
    CHECK_MESSAGE(waitFor(project, editor, [&] { return !editor.document.path().has_value(); }),
                  "removing the open scene's file should make the document untitled");
    CHECK_MESSAGE(editor.document.dirty(), "a detached document should have unsaved changes");

    // Duplicates get their own identity.
    panel.duplicateItem(content / "props" / "box.gltf", editor.diagnostics);
    const AssetGuid copyGuid = sidecarGuid(content / "props" / "box copy.gltf");
    CHECK_MESSAGE((copyGuid && copyGuid != modelGuid), "a duplicate should get a fresh GUID");
}

// Every checked-in scene, whether current or migrated, loads and round-trips
// in the current document form.
TEST_CASE("AssetReferenceWorkflow: checked in scenes") {
    const std::filesystem::path assets = ZENITH_ASSET_DIR;
    const std::filesystem::path sceneDirectory = std::filesystem::path(ZENITH_SOURCE_DIR) / "projects" / "scenes";
    size_t loadedScenes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(sceneDirectory)) {
        if (entry.path().extension() != ".zenith-scene") {
            continue;
        }
        const std::string name = entry.path().filename().string();
        const SceneDocumentOptions options{.assetRoot = assets};
        AssetManager assetManager;
        Scene scene;
        const SceneDocumentResult loaded = SceneDocument::load(entry.path(), assetManager, options, scene);
        CHECK_MESSAGE(loaded.success, (name + " should load through migration: " + loaded.error));
        std::string upgraded;
        CHECK_MESSAGE(SceneDocument::serialize(scene, assetManager, options, upgraded).success,
                      (name + " should serialize"));
        CHECK_MESSAGE(nlohmann::json::parse(upgraded).value("version", 0u) == SceneDocument::currentVersion,
                      (name + " should save as the current version"));
        Scene again;
        std::string roundTrip;
        CHECK_MESSAGE((SceneDocument::deserialize(upgraded, assetManager, options, again).success &&
                       SceneDocument::serialize(again, assetManager, options, roundTrip).success &&
                       roundTrip == upgraded),
                      (name + " should round-trip in the current version"));
        ++loadedScenes;
    }
    CHECK_MESSAGE(loadedScenes >= 3, "the checked-in scenes should be found");
}
} // namespace

TEST_SUITE_END();
