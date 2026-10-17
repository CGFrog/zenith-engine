#include "engine/assets/Assets.hpp"
#include "engine/assets/MaterialAssetFile.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneAssetReferences.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

TEST_SUITE_BEGIN("MaterialAsset");

// Material files (.zmat): the document format, AssetManager loading, reload,
// and collection, the material override's material_asset field, the asset
// reload that keeps edited texture paths from being saved over, and the
// scene lease of particle sprites.

namespace {

// A temporary project: textures/ holds two images, materials/ the material.
struct MaterialProject {
    const testsupport::TempDirectory directory;
    const std::filesystem::path root;

    explicit MaterialProject(std::string_view name)
        : directory(name)
        , root(directory.path()) {
        std::filesystem::create_directories(root / "textures");
        std::filesystem::create_directories(root / "materials");
        const std::filesystem::path decals = std::filesystem::path(ZENITH_ASSET_DIR) / "showcase" / "decals";
        std::filesystem::copy_file(decals / "decal_plate.png", root / "textures" / "plate.png");
        std::filesystem::copy_file(decals / "decal_plate_normal.png", root / "textures" / "plate_normal.png");
    }

    std::filesystem::path material() const {
        return root / "materials" / "stone.zmat";
    }

    SceneDocumentOptions options() const {
        return {.assetRoot = root};
    }
};

MaterialFile stoneMaterial() {
    MaterialFile material;
    material.baseColor = {0.5f, 0.25f, 1.0f, 1.0f};
    material.roughness = 0.75f;
    material.metallic = 0.25f;
    material.alphaMode = MaterialAlphaMode::Mask;
    material.alphaCutoff = 0.3f;
    material.doubleSided = true;
    material.textures[static_cast<size_t>(MaterialTextureSlot::BaseColor)] = {.path = "../textures/plate.png"};
    material.textures[static_cast<size_t>(MaterialTextureSlot::Normal)] = {.path = "../textures/plate_normal.png",
                                                                           .guid = AssetGuid(1, 2)};
    // The base color image again, read as linear data this time.
    material.textures[static_cast<size_t>(MaterialTextureSlot::MetallicRoughness)] = {.path = "../textures/plate.png"};
    material.textures[static_cast<size_t>(MaterialTextureSlot::Occlusion)] = {.path = "../textures/missing.png"};
    return material;
}

TEST_CASE("MaterialAsset: file format") {
    const MaterialFile material = stoneMaterial();
    MaterialFile read;
    const MaterialFileResult parsed = parseMaterialFile(serializeMaterialFile(material), read);
    CHECK_MESSAGE((parsed.success && read == material), ("a material file should round trip: " + parsed.error));

    MaterialFile defaults;
    CHECK_MESSAGE((parseMaterialFile(R"({"version": 1})", defaults).success && defaults == MaterialFile{}),
                  "missing values should keep their defaults");
    MaterialFile rejected;
    CHECK_MESSAGE(!parseMaterialFile(R"({"version": 2})", rejected), "a newer version should be rejected");
    CHECK_MESSAGE(!parseMaterialFile(R"({"roughness": -1})", rejected), "negative factors should be rejected");
    CHECK_MESSAGE(!parseMaterialFile(R"({"alpha_mode": "glow"})", rejected), "unknown alpha modes should be rejected");
    CHECK_MESSAGE(!parseMaterialFile(R"({"normal_texture": {"path": "a.png", "guid": "xyz"}})", rejected),
                  "a malformed texture GUID should be rejected");
    CHECK_MESSAGE(!parseMaterialFile("[1, 2]", rejected), "a document that is not an object should be rejected");
}

TEST_CASE("MaterialAsset: water") {
    MaterialFile pond;
    pond.baseColor = {0.05f, 0.2f, 0.25f, 1.0f};
    pond.roughness = 0.05f;
    pond.water = {.enabled = true,
                  .waveLength = 2.5f,
                  .waveSteepness = 0.4f,
                  .waveSpeed = 0.7f,
                  .shoreFade = 0.1f,
                  .absorptionColor = {0.3f, 0.7f, 0.75f},
                  .absorptionDistance = 1.2f,
                  .fallbackDepth = 0.5f};
    MaterialFile read;
    const MaterialFileResult parsed = parseMaterialFile(serializeMaterialFile(pond), read);
    CHECK_MESSAGE((parsed.success && read == pond), ("a water material should round trip: " + parsed.error));
    CHECK_MESSAGE(serializeMaterialFile(MaterialFile{}).find("water") == std::string::npos,
                  "a material that is not water writes no water");

    MaterialFile defaults;
    CHECK_MESSAGE((parseMaterialFile(R"({"water": {}})", defaults).success && defaults.water.enabled &&
                   defaults.water.waveLength == MaterialWater{}.waveLength),
                  "an empty water object makes water with the default settings");
    MaterialFile rejected;
    CHECK_MESSAGE(!parseMaterialFile(R"({"water": true})", rejected), "water must be an object");
    CHECK_MESSAGE(!parseMaterialFile(R"({"water": {"wave_length": 0}})", rejected), "a wave length must be positive");
    CHECK_MESSAGE(!parseMaterialFile(R"({"water": {"absorption_distance": -1}})", rejected),
                  "an absorption distance must be positive");
}

TEST_CASE("MaterialAsset: load reload and collect") {
    const MaterialProject project("zenith_material_asset_load");
    CHECK_MESSAGE(writeMaterialFile(project.material(), stoneMaterial()).success, "writing a material should succeed");

    Logger logger;
    AssetManager assets(&logger);
    const AssetLoadResult<MaterialHandle> loaded = assets.loadMaterial(project.material());
    const MaterialAsset* material = assets.material(loaded.handle);
    CHECK_MESSAGE((loaded && material != nullptr), ("a material file should load: " + loaded.error));
    if (material == nullptr) {
        return;
    }
    CHECK_MESSAGE((material->roughness == 0.75f && material->metallic == 0.25f &&
                   material->alphaMode == MaterialAlphaMode::Mask && material->doubleSided),
                  "a loaded material should carry the file's values");
    const TextureAsset* baseColor = assets.texture(material->baseColorTexture);
    const TextureAsset* normal = assets.texture(material->normalTexture);
    const TextureAsset* data = assets.texture(material->metallicRoughnessTexture);
    CHECK_MESSAGE((baseColor != nullptr && baseColor->usage == TextureUsage::Color && !baseColor->blockCompressed()),
                  "the base color map should load as RGBA8 color");
    CHECK_MESSAGE((normal != nullptr && normal->usage == TextureUsage::Normal),
                  "the normal map should load as a normal map");
    CHECK_MESSAGE((data != nullptr && data->usage == TextureUsage::Data &&
                   material->metallicRoughnessTexture != material->baseColorTexture),
                  "the same image in a data slot should be a separate linear texture");
    CHECK_MESSAGE(!material->occlusionTexture, "a slot whose image is missing should stay empty");
    CHECK_MESSAGE((assets.materialSource(loaded.handle).has_value() &&
                   std::filesystem::equivalent(assets.materialSource(loaded.handle)->path, project.material())),
                  "a loaded material should know its file");
    CHECK_MESSAGE(assets.loadMaterial(project.material()).handle == loaded.handle, "loads should be cached by path");

    MaterialFile changed = stoneMaterial();
    changed.roughness = 0.125f;
    CHECK_MESSAGE(writeMaterialFile(project.material(), changed).success, "rewriting the material should succeed");
    const AssetLoadResult<MaterialHandle> reloaded = assets.reloadMaterial(project.material());
    CHECK_MESSAGE(
        (reloaded && reloaded.handle != loaded.handle && assets.material(reloaded.handle)->roughness == 0.125f),
        "a reload should make a new material with the new values");
    CHECK_MESSAGE(assets.material(loaded.handle) != nullptr, "the previous material should stay valid for its users");
    CHECK_MESSAGE(assets.loadMaterial(project.material()).handle == reloaded.handle,
                  "a reload should update later loads");

    {
        const AssetLease<MaterialHandle> lease = assets.lease(reloaded.handle);
        assets.collectUnused(true);
        CHECK_MESSAGE(assets.material(loaded.handle) == nullptr, "an unleased material should be collected");
        CHECK_MESSAGE((assets.material(reloaded.handle) != nullptr &&
                       assets.texture(assets.material(reloaded.handle)->normalTexture) != nullptr),
                      "a leased material should keep itself and its textures");
    }
    assets.collectUnused(true);
    CHECK_MESSAGE(assets.material(reloaded.handle) == nullptr, "a released material should be collected");
    const AssetLoadResult<MaterialHandle> again = assets.loadMaterial(project.material());
    CHECK_MESSAGE(
        (again && assets.material(again.handle) != nullptr && assets.material(again.handle)->roughness == 0.125f),
        "a collected material should load from its file again");

    const AssetLoadResult<MaterialHandle> missing = assets.loadMaterial(project.root / "materials" / "none.zmat");
    CHECK_MESSAGE((!missing && !missing.error.empty()), "a missing material file should fail with an error");
}

TEST_CASE("MaterialAsset: material override field") {
    const MaterialProject project("zenith_material_asset_override");
    CHECK_MESSAGE(writeMaterialFile(project.material(), stoneMaterial()).success, "writing a material should succeed");
    Logger logger;
    AssetManager assets(&logger);
    Scene scene;
    const Entity entity = scene.createEntity("Wall");
    const SceneDocumentOptions options = project.options();
    const ComponentType& type = *builtInComponentTypes()->find("material_override");

    // A version 1 document (no $version) reads unchanged.
    const nlohmann::json versionOne = {{"material_asset", {{"path", "materials/stone.zmat"}}},
                                       {"roughness_factor", 0.5}};
    const SceneDocumentResult loaded =
        type.load(versionOne, scene, entity, ComponentLoadContext{.assets = assets, .options = options});
    const auto* component = scene.registry().try_get<MaterialOverrideComponent>(entity);
    CHECK_MESSAGE((loaded.success && component != nullptr),
                  ("a version 1 material override should load: " + loaded.error));
    if (component == nullptr) {
        return;
    }
    CHECK_MESSAGE((component->materialAsset == "materials/stone.zmat" &&
                   assets.material(component->material) != nullptr && component->roughnessFactor == 0.5f),
                  "loading should fill the material from material_asset and keep the factors");
    CHECK_MESSAGE(type.assetLoaded(component, "material_asset") == true, "the material field should report it loaded");

    nlohmann::json saved = nlohmann::json::object();
    CHECK_MESSAGE(type.save(scene, entity, ComponentSaveContext{.assets = assets, .options = options}, saved).success,
                  "a material override should save");
    CHECK_MESSAGE((saved.value("$version", 0) == 2 && saved.at("material_asset").at("path") == "materials/stone.zmat"),
                  "saving should write version 2 and the material reference");

    const nlohmann::json missing = {{"material_asset", {{"path", "materials/none.zmat"}}}};
    const Entity other = scene.createEntity("Other");
    CHECK_MESSAGE(!type.load(missing, scene, other, ComponentLoadContext{.assets = assets, .options = options}),
                  "a missing material file should fail the component");
}

TEST_CASE("MaterialAsset: reload assets keeps edited paths") {
    const MaterialProject project("zenith_material_asset_reload");
    CHECK_MESSAGE(writeMaterialFile(project.material(), stoneMaterial()).success, "writing a material should succeed");
    Logger logger;
    AssetManager assets(&logger);
    Scene scene;
    const Entity entity = scene.createEntity("Decal");
    const SceneDocumentOptions options = project.options();
    const ComponentType& decalType = *builtInComponentTypes()->find("decal");
    const nlohmann::json decalDocument = {{"half_extents", {1.0, 1.0, 1.0}},
                                          {"albedo_asset", {{"path", "textures/plate.png"}}}};
    CHECK_MESSAGE(
        decalType.load(decalDocument, scene, entity, ComponentLoadContext{.assets = assets, .options = options})
            .success,
        "a decal should load");
    auto& decal = scene.registry().get<DecalComponent>(entity);
    const TextureHandle original = decal.albedoTexture;
    const auto savedPath = [&] {
        nlohmann::json saved = nlohmann::json::object();
        decalType.save(scene, entity, ComponentSaveContext{.assets = assets, .options = options}, saved);
        // An empty path is written as an empty string, a set one as a reference.
        const auto found = saved.find("albedo_asset");
        return found != saved.end() && found->is_object() ? found->value("path", std::string{}) : std::string{};
    };

    // An inspector edits the path in place; the loaded handle still names
    // the old file, which saving would write back.
    decal.albedoAsset = "textures/plate_normal.png";
    CHECK_MESSAGE(savedPath() == "textures/plate.png", "a stale handle's path is what saving writes");
    CHECK_MESSAGE(decalType.reloadAssets(&decal, assets, options).success, "reloading the edited path should succeed");
    CHECK_MESSAGE((decal.albedoTexture != original && savedPath() == "textures/plate_normal.png"),
                  "after reloading, saving should write the edited path");
    const TextureHandle current = decal.albedoTexture;
    CHECK_MESSAGE((decalType.reloadAssets(&decal, assets, options).success && decal.albedoTexture == current),
                  "an unchanged path should keep its handle");

    decal.albedoAsset.clear();
    CHECK_MESSAGE(
        (decalType.reloadAssets(&decal, assets, options).success && !decal.albedoTexture && savedPath().empty()),
        "an emptied path should clear its handle");

    decal.albedoAsset = "textures/none.png";
    CHECK_MESSAGE((!decalType.reloadAssets(&decal, assets, options) && !decal.albedoTexture),
                  "a path naming no file should fail and clear the handle");

    // A runtime texture code assigned has no file and survives an edit of
    // another field.
    TextureAsset pixel{.width = 1, .height = 1, .rgba = {255, 255, 255, 255}};
    decal.albedoAsset.clear();
    decal.albedoTexture = assets.createTexture(std::move(pixel), "white").handle;
    const TextureHandle runtime = decal.albedoTexture;
    CHECK_MESSAGE((decalType.reloadAssets(&decal, assets, options).success && decal.albedoTexture == runtime),
                  "a runtime texture with an empty path should be kept");

    // The material binding behaves alike.
    const ComponentType& overrideType = *builtInComponentTypes()->find("material_override");
    MaterialOverrideComponent values;
    values.materialAsset = "materials/stone.zmat";
    CHECK_MESSAGE(
        (overrideType.reloadAssets(&values, assets, options).success && assets.material(values.material) != nullptr),
        "reloading should load an assigned material");
    values.materialAsset.clear();
    CHECK_MESSAGE((overrideType.reloadAssets(&values, assets, options).success && !values.material),
                  "clearing the material path should clear the material");
}

TEST_CASE("MaterialAsset: particle sprites are leased") {
    const MaterialProject project("zenith_material_asset_sprites");
    Logger logger;
    AssetManager assets(&logger);
    Scene scene;
    const Entity entity = scene.createEntity("Sparks");
    const ComponentType& emitterType = *builtInComponentTypes()->find("particle_emitter");
    const SceneDocumentOptions options = project.options();
    CHECK_MESSAGE(emitterType
                      .load({{"texture_asset", {{"path", "textures/plate.png"}}}}, scene, entity,
                            ComponentLoadContext{.assets = assets, .options = options})
                      .success,
                  "an emitter with a sprite should load");
    const TextureHandle sprite = scene.registry().get<ParticleEmitterComponent>(entity).texture;
    {
        const AssetLeaseSet leases = assets.leaseSceneAssets(scene);
        assets.collectUnused(true);
        CHECK_MESSAGE(assets.texture(sprite) != nullptr, "a leased scene should keep its particle sprites");
    }
    assets.collectUnused(true);
    CHECK_MESSAGE(assets.texture(sprite) == nullptr, "a sprite nothing leases should be collected");
}
} // namespace

TEST_SUITE_END();
