#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/AssetReferenceField.hpp"
#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/project/MaterialDocument.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/project/ThumbnailTextures.hpp"
#include "engine/assets/MaterialAssetFile.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

// Asset fields in the editor: catalog kinds and what each asset kind offers,
// the widget's drop, Clear, undo, redo, and multi-object edits, the commit of
// a texture field whose handle is loaded (the edit must not be saved over),
// GPU thumbnails, and the editor's material document.

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr const char* propertiesWindow = "Asset field test";

// A project folder with two images, an HDR stand-in, a sound, and a
// material, listed by a ProjectAssets service.
struct TestProject {
    std::filesystem::path root;
    std::unique_ptr<ProjectAssets> assets;

    explicit TestProject(std::string_view name, EditorDiagnostics& diagnostics)
        : root(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "textures");
        std::filesystem::create_directories(root / "sounds");
        std::filesystem::create_directories(root / "materials");
        const std::filesystem::path decals = std::filesystem::path(ZENITH_ASSET_DIR) / "showcase" / "decals";
        std::filesystem::copy_file(decals / "decal_plate.png", root / "textures" / "plate.png");
        std::filesystem::copy_file(decals / "decal_arrow.png", root / "textures" / "arrow.png");
        std::ofstream(root / "textures" / "sky.hdr") << "not decoded by these tests";
        std::ofstream(root / "sounds" / "step.wav") << "not decoded by these tests";
        writeMaterialFile(root / "materials" / "stone.zmat", MaterialFile{});
        assets = std::make_unique<ProjectAssets>(root, root, root / "asset-browser.json",
                                                 AssetCatalogOptions{.watchFiles = false, .guidSeed = 7});
        assets->refresh(diagnostics);
    }

    ~TestProject() {
        assets.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    AssetReferenceFieldContext fieldContext() const {
        return {.catalog = assets->catalog(), .assetRoot = root};
    }
};

struct Fixture : EditorTestFixture {
    TestProject project;
    EntityPropertiesView view{componentLayout};
    std::vector<SceneEntityId> decals;

    explicit Fixture(std::string_view name)
        : EditorTestFixture(EditorTestFixtureOptions{.documentRoot = std::filesystem::temp_directory_path() / name})
        , project(name, diagnostics) {
        projectAssets = project.assets.get();
        // The first decal shows a loaded plate, the second has no map.
        decals.push_back(addDecal("textures/plate.png"));
        decals.push_back(addDecal(""));
        selection.selectOnly(decals.front());
    }

    SceneEntityId addDecal(const std::string& albedo) {
        const Entity entity = scene.createEntity("Decal");
        nlohmann::json component = {{"half_extents", {1.0, 1.0, 1.0}}};
        if (!albedo.empty()) {
            component["albedo_asset"] = {{"path", albedo}};
        }
        const SceneDocumentResult loaded = decalType().load(
            component, scene, entity, ComponentLoadContext{.assets = assets, .options = document.options()});
        expect(loaded.success, "a decal should load: " + loaded.error);
        return scene.id(entity);
    }

    const ComponentType& decalType() const {
        return *componentTypesFor(document.options()).find("decal");
    }

    DecalComponent& decal(size_t index) {
        return scene.registry().get<DecalComponent>(scene.findEntity(decals[index]));
    }

    std::string loadedAlbedo(size_t index) {
        const std::optional<TextureAssetSource> source = assets.textureSource(decal(index).albedoTexture);
        return source ? source->path.filename().string() : std::string{};
    }

    void drawProperties(std::span<const SceneEntityId> shown, const std::function<void()>& before = {}) {
        HeadlessImGuiFrame frame(propertiesWindow);
        if (before) {
            before();
        }
        EditorContext editor = context();
        view.drawComponent(editor, decalType(), shown, {});
    }
};

ImGuiID propertiesItem(const char* field, const char* item) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(propertiesWindow);
    return window == nullptr ? 0 : HeadlessImGuiContext::itemId(*window, "decal", field, item);
}

bool hoverItem(ImGuiID id, float x, const std::function<void()>& draw) {
    ImGuiIO& io = ImGui::GetIO();
    for (float y = 0.0f; y < io.DisplaySize.y; y += 2.0f) {
        io.AddMousePosEvent(x, y);
        draw();
        if (ImGui::GetCurrentContext()->HoveredId == id) {
            return true;
        }
    }
    return false;
}

// An extern drag source standing in for an Asset Browser tile drops path on
// the hovered field: accepted on the first frame, delivered on the next.
void dropAsset(const std::function<void(const std::function<void()>&)>& draw, const char* payloadType,
               const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    const std::string utf8(text.begin(), text.end());
    draw([&] {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern | ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload(payloadType, utf8.c_str(), utf8.size() + 1);
            ImGui::EndDragDropSource();
        }
    });
    draw({});
}

void testCatalogKinds() {
    expect(editorAssetKind("sky.HDR") == EditorAssetKind::Texture &&
               editorAssetKind("cooked.ktx2") == EditorAssetKind::Texture,
           "HDR and KTX2 images should be textures");
    expect(editorAssetKind("stone.zmat") == EditorAssetKind::Material, ".zmat files should be materials");
    expect(editorAssetKindsFor("environment") == std::vector{EditorAssetKind::Texture} &&
               editorAssetKindsFor("texture") == std::vector{EditorAssetKind::Texture} &&
               editorAssetKindsFor("audio") == std::vector{EditorAssetKind::Audio} &&
               editorAssetKindsFor("material") == std::vector{EditorAssetKind::Material} &&
               editorAssetKindsFor("model") == std::vector{EditorAssetKind::Model},
           "each asset kind should offer its catalog kind");
    expect(editorAssetKindsFor("asset").size() == 7, "a generic asset field should offer every kind");
    expect(std::string_view(assetDragPayloadFor(EditorAssetKind::Texture)) == textureAssetPayload &&
               std::string_view(assetDragPayloadFor(EditorAssetKind::Audio)) == audioAssetPayload &&
               std::string_view(assetDragPayloadFor(EditorAssetKind::Material)) == materialAssetPayload &&
               assetDragPayloadFor(EditorAssetKind::Scene) == nullptr,
           "textures, sounds, and materials should drag with their own payloads");
}

void testValuesAndCandidates() {
    EditorDiagnostics diagnostics;
    const TestProject project("zenith_asset_field_candidates", diagnostics);
    const AssetReferenceFieldContext context = project.fieldContext();
    expect(projectAssetValue(project.root, project.root / "textures" / "plate.png") == "textures/plate.png",
           "a value should be relative to the asset root with forward slashes");
    expect(!projectAssetValue(project.root / "textures", project.root / "sounds" / "step.wav"),
           "a file outside the asset root should have no value");

    const auto names = [&](std::string_view kind, std::string_view search) {
        std::vector<std::string> found;
        for (const EditorAssetRecord* asset : assetFieldCandidates(context, kind, search)) {
            found.push_back(asset->absolutePath.filename().string());
        }
        return found;
    };
    expect(names("texture", "") == std::vector<std::string>{"arrow.png", "plate.png", "sky.hdr"},
           "a texture field should offer every image, HDR included");
    expect(names("environment", "SKY") == std::vector<std::string>{"sky.hdr"},
           "the search should match file names ignoring case");
    expect(names("audio", "") == std::vector<std::string>{"step.wav"}, "an audio field should offer sounds");
    expect(names("material", "") == std::vector<std::string>{"stone.zmat"}, "a material field should offer materials");

    expect(assetReferenceState(context, "", std::nullopt) == AssetReferenceState::None, "empty should read None");
    expect(assetReferenceState(context, "textures/plate.png", true) == AssetReferenceState::Present,
           "a listed, loaded asset should be present");
    expect(assetReferenceState(context, "textures/gone.png", std::nullopt) == AssetReferenceState::Missing &&
               assetReferenceState(context, "textures/plate.png", false) == AssetReferenceState::Missing,
           "an unlisted or unloaded asset should be missing");
}

void testDropUndoRedoAndMultiEdit() {
    Fixture fixture("zenith_asset_field_drop");
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    const std::vector<SceneEntityId> both = fixture.decals;
    const auto draw = [&](const std::function<void()>& before) { fixture.drawProperties(both, before); };
    draw({});
    expect(hoverItem(propertiesItem("albedo_asset", "##value"), 120.0f, [&] { draw({}); }),
           "the albedo value box should be hoverable");
    dropAsset(draw, textureAssetPayload, fixture.project.root / "textures" / "arrow.png");
    expect(fixture.decal(0).albedoAsset == "textures/arrow.png" && fixture.decal(1).albedoAsset == "textures/arrow.png",
           "dropping on a mixed field should assign every selected decal");
    expect(fixture.history.size() == 1, "the drop should be one undo step");
    expect(fixture.loadedAlbedo(0) == "arrow.png" && fixture.loadedAlbedo(1) == "arrow.png",
           "the assigned texture should be loaded, not the previous one");

    fixture.editing().undo();
    expect(fixture.decal(0).albedoAsset == "textures/plate.png" && fixture.loadedAlbedo(0) == "plate.png" &&
               fixture.decal(1).albedoAsset.empty() && !fixture.decal(1).albedoTexture,
           "undo should restore each decal's own map");
    fixture.editing().redo();
    expect(fixture.decal(0).albedoAsset == "textures/arrow.png" && fixture.loadedAlbedo(1) == "arrow.png",
           "redo should assign the map again");

    // A payload of another kind is refused.
    dropAsset(draw, audioAssetPayload, fixture.project.root / "sounds" / "step.wav");
    expect(fixture.decal(0).albedoAsset == "textures/arrow.png" && fixture.history.size() == 1,
           "a texture field should refuse a sound");
}

void testClearAndCommitOfLoadedTexture() {
    Fixture fixture("zenith_asset_field_clear");
    HeadlessImGuiContext imgui;
    const SceneEntityId first = fixture.decals.front();
    const auto draw = [&] { fixture.drawProperties(std::span(&first, 1)); };
    draw();
    HeadlessImGuiContext::activateNextFrame(propertiesItem("albedo_asset", "Clear"));
    draw();
    expect(fixture.decal(0).albedoAsset.empty() && !fixture.decal(0).albedoTexture && fixture.history.size() == 1,
           "Clear should empty a loaded texture field as one edit");
    fixture.editing().undo();
    expect(fixture.decal(0).albedoAsset == "textures/plate.png" && fixture.loadedAlbedo(0) == "plate.png",
           "undo should restore the cleared texture");
}

void testThumbnailTextures() {
    EditorDiagnostics diagnostics;
    const TestProject project("zenith_asset_field_thumbnails", diagnostics);
    RenderRuntime rendering;
    ThumbnailCache cache;
    ThumbnailTextures textures(cache, 1);
    const std::filesystem::path plate = project.root / "textures" / "plate.png";
    const std::filesystem::path arrow = project.root / "textures" / "arrow.png";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (textures.size() == 0 && std::chrono::steady_clock::now() < deadline) {
        textures.texture(rendering, plate);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(textures.size() == 1, "a decoded thumbnail should become a UI image");
    while (textures.size() == 1 && cache.status(arrow).value_or(ThumbnailStatus{}).state != ThumbnailState::Ready &&
           std::chrono::steady_clock::now() < deadline) {
        textures.texture(rendering, arrow);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    textures.texture(rendering, arrow);
    expect(textures.size() == 1, "the cache should stay within its capacity, evicting the least recent image");
    textures.release(rendering);
    expect(textures.size() == 0, "release should destroy every image");
}

void testMaterialDocument() {
    EditorDiagnostics diagnostics;
    const TestProject project("zenith_asset_field_material_document", diagnostics);
    const std::filesystem::path file = project.root / "materials" / "stone.zmat";
    const MaterialDocumentLocation location{.assetRoot = project.root,
                                            .database = project.assets->assetDatabase().get()};
    MaterialDocument document;
    expect(loadMaterialDocument(file, location, document).success, "a new material should load as a document");
    document.roughness = 0.25f;
    document.baseColorTexture = "textures/plate.png";
    expect(saveMaterialDocument(file, location, document).success, "a material document should save");
    MaterialFile written;
    expect(readMaterialFile(file, written).success, "the saved file should read back");
    const MaterialTextureReference& baseColor = written.textures[static_cast<size_t>(MaterialTextureSlot::BaseColor)];
    expect(baseColor.path == "../textures/plate.png" && baseColor.guid, "textures should be saved relative to the "
                                                                        "material, with their GUID");
    MaterialDocument reread;
    expect(loadMaterialDocument(file, location, reread).success && reread == document,
           "a saved document should read back unchanged");
    expect(validateFieldDescriptors({materialDocumentFields().begin(), materialDocumentFields().end()}).empty(),
           "the material document's fields should validate");
}

void testMaterialReloadRepointsUsers() {
    EditorDiagnostics diagnostics;
    const TestProject project("zenith_asset_field_material_reload", diagnostics);
    const std::filesystem::path file = project.root / "materials" / "stone.zmat";
    Logger logger;
    AssetManager assets(&logger);
    Scene scene;
    const Entity wall = scene.createEntity("Wall");
    const Entity tinted = scene.createEntity("Tinted");
    const MaterialHandle original = assets.loadMaterial(file).handle;
    scene.registry().emplace<MaterialOverrideComponent>(
        wall, MaterialOverrideComponent{.materialAsset = "materials/stone.zmat", .material = original});
    scene.registry().emplace<MaterialOverrideComponent>(tinted, MaterialOverrideComponent{.roughnessFactor = 0.5f});

    MaterialFile changed;
    changed.roughness = 0.125f;
    expect(writeMaterialFile(file, changed).success, "rewriting the material should succeed");
    Scene* scenes[] = {&scene, nullptr};
    expect(reloadProjectMaterial(assets, scenes, diagnostics, file) == 1, "one user should be re-pointed");
    const MaterialHandle current = scene.registry().get<MaterialOverrideComponent>(wall).material;
    expect(current != original && assets.material(current)->roughness == 0.125f,
           "the user should draw the reloaded material");
    expect(!scene.registry().get<MaterialOverrideComponent>(tinted).material,
           "an override without the material should be left alone");
}
} // namespace

int main() {
    testCatalogKinds();
    testValuesAndCandidates();
    testDropUndoRedoAndMultiEdit();
    testClearAndCommitOfLoadedTexture();
    testThumbnailTextures();
    testMaterialDocument();
    testMaterialReloadRepointsUsers();
    if (failures != 0) {
        std::cerr << failures << " asset field test(s) failed\n";
        return 1;
    }
    std::cout << "Asset field tests passed\n";
    return 0;
}
