// KHR material extensions (clearcoat, sheen, specular, transmission, volume,
// ior): glTF import, texture slot usages, the GPU material record packing, the
// sheen albedo table, and the generated material gallery content.
#include "engine/assets/Assets.hpp"
#include "engine/assets/ModelImporter.hpp"
#include "engine/graphics/EnvironmentLighting.hpp"
#include "engine/graphics/MaterialRecord.hpp"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(float first, float second, float tolerance = 1.0e-5f) {
    return std::abs(first - second) <= tolerance;
}

bool near(const glm::vec3& first, const glm::vec3& second, float tolerance = 1.0e-5f) {
    return near(first.x, second.x, tolerance) && near(first.y, second.y, tolerance) &&
           near(first.z, second.z, tolerance);
}

constexpr int extensionImageCount = 8;

// A 1x1 RGB PNG.
constexpr unsigned char pixelPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00,
    0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xdf, 0xe0, 0x00, 0x00, 0x04, 0x41, 0x01, 0xc0, 0x0c,
    0xd1, 0xed, 0x91, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

// One quad (two triangles) with two materials: every extension with a
// distinct image file per texture slot, and a plain metallic-roughness material.
std::string extensionModel() {
    std::string images;
    std::string textures;
    for (int index = 0; index < extensionImageCount; ++index) {
        images += std::string(index > 0 ? ", " : "") + "{ \"uri\": \"slot" + std::to_string(index) + ".png\" }";
        textures += std::string(index > 0 ? ", " : "") + "{ \"source\": " + std::to_string(index) + " }";
    }
    return R"({
  "asset": { "version": "2.0" },
  "extensionsUsed": ["KHR_materials_clearcoat", "KHR_materials_sheen", "KHR_materials_specular",
                     "KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_ior"],
  "scene": 0,
  "scenes": [{ "nodes": [0] }],
  "nodes": [{ "mesh": 0 }],
  "meshes": [{ "primitives": [
    { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 },
    { "attributes": { "POSITION": 0 }, "indices": 1, "material": 1 }
  ] }],
  "materials": [{
    "extensions": {
      "KHR_materials_clearcoat": { "clearcoatFactor": 0.8, "clearcoatRoughnessFactor": 0.1,
        "clearcoatTexture": { "index": 0 }, "clearcoatRoughnessTexture": { "index": 1 },
        "clearcoatNormalTexture": { "index": 2, "scale": 0.5 } },
      "KHR_materials_sheen": { "sheenColorFactor": [0.9, 0.4, 0.2], "sheenRoughnessFactor": 0.3,
        "sheenColorTexture": { "index": 3 }, "sheenRoughnessTexture": { "index": 4 } },
      "KHR_materials_specular": { "specularFactor": 0.7, "specularColorFactor": [1.0, 0.5, 0.25],
        "specularTexture": { "index": 5 }, "specularColorTexture": { "index": 6 } },
      "KHR_materials_transmission": { "transmissionFactor": 0.9, "transmissionTexture": { "index": 7 } },
      "KHR_materials_volume": { "thicknessFactor": 0.25, "attenuationColor": [0.5, 0.25, 1.0],
        "attenuationDistance": 2.0 },
      "KHR_materials_ior": { "ior": 1.33 }
    }
  }, {
    "pbrMetallicRoughness": { "metallicFactor": 0.0 }
  }],
  "images": [)" +
           images +
           R"(],
  "textures": [)" +
           textures + R"(],
  "buffers": [{ "byteLength": 72, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAACAvwAAgL8AAIA/AACAPwAAgL8AAIA/AAAAAAAAgL8AAIC/AAABAAIAAAACAAMAAAADAAEAAQADAAIA" }],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 48 },
    { "buffer": 0, "byteOffset": 48, "byteLength": 24 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1, -1, -1], "max": [1, 1, 1] },
    { "bufferView": 1, "componentType": 5123, "count": 12, "type": "SCALAR" }
  ]
})";
}

const MaterialAsset* primitiveMaterial(const AssetManager& assets, ModelHandle handle, size_t primitive) {
    const ModelAsset* model = assets.model(handle);
    const MeshAsset* mesh = model && !model->nodes.empty() ? assets.mesh(model->nodes[0].mesh) : nullptr;
    if (!mesh || primitive >= mesh->primitives.size()) {
        return nullptr;
    }
    return assets.material(mesh->primitives[primitive].material);
}

bool hasUsage(const AssetManager& assets, TextureHandle handle, TextureUsage usage) {
    const TextureAsset* texture = assets.texture(handle);
    return texture != nullptr && texture->usage == usage;
}

void testImport() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith-material-extension-tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const std::filesystem::path path = root / "extensions.gltf";
    {
        std::ofstream file(path, std::ios::binary);
        file << extensionModel();
    }
    for (int index = 0; index < extensionImageCount; ++index) {
        std::ofstream image(root / ("slot" + std::to_string(index) + ".png"), std::ios::binary);
        image.write(reinterpret_cast<const char*>(pixelPng), sizeof(pixelPng));
    }

    AssetManager assets;
    const AssetLoadResult<ModelHandle> loaded = assets.loadModel(path);
    expect(static_cast<bool>(loaded), "a model using every material extension loads: " + loaded.error);
    const MaterialAsset* layered = primitiveMaterial(assets, loaded.handle, 0);
    const MaterialAsset* plain = primitiveMaterial(assets, loaded.handle, 1);
    expect(layered && plain, "both primitives keep their materials");
    if (layered) {
        expect(near(layered->ior, 1.33f), "KHR_materials_ior imports the index of refraction");
        expect(near(layered->clearcoat.factor, 0.8f) && near(layered->clearcoat.roughness, 0.1f) &&
                   near(layered->clearcoat.normalScale, 0.5f),
               "clearcoat factors and normal scale import");
        expect(near(layered->sheen.color, {0.9f, 0.4f, 0.2f}) && near(layered->sheen.roughness, 0.3f),
               "sheen color and roughness import");
        expect(near(layered->specular.factor, 0.7f) && near(layered->specular.color, {1.0f, 0.5f, 0.25f}),
               "specular factor and color import");
        expect(near(layered->transmission.factor, 0.9f) && near(layered->transmission.thickness, 0.25f) &&
                   near(layered->transmission.attenuationColor, {0.5f, 0.25f, 1.0f}) &&
                   near(layered->transmission.attenuationDistance, 2.0f),
               "transmission and volume factors import");
        expect(layered->translucent(), "transmissive materials draw after opaque geometry");
        expect(hasUsage(assets, layered->clearcoat.texture, TextureUsage::Data) &&
                   hasUsage(assets, layered->clearcoat.roughnessTexture, TextureUsage::Data) &&
                   hasUsage(assets, layered->clearcoat.normalTexture, TextureUsage::Normal),
               "clearcoat textures are linear data and a normal map");
        expect(hasUsage(assets, layered->sheen.colorTexture, TextureUsage::Color) &&
                   hasUsage(assets, layered->sheen.roughnessTexture, TextureUsage::Data),
               "sheen color is sRGB color and its roughness linear data");
        expect(hasUsage(assets, layered->specular.texture, TextureUsage::Data) &&
                   hasUsage(assets, layered->specular.colorTexture, TextureUsage::Color),
               "specular strength is linear data and its color sRGB color");
        expect(hasUsage(assets, layered->transmission.texture, TextureUsage::Data),
               "the transmission texture is linear data");
    }
    if (plain) {
        const MaterialAsset defaults;
        expect(near(plain->ior, 1.5f) && plain->clearcoat.factor == 0.0f && plain->sheen.color == glm::vec3(0.0f) &&
                   plain->specular.factor == 1.0f && plain->specular.color == glm::vec3(1.0f) &&
                   plain->transmission.factor == 0.0f && plain->transmission.thickness == 0.0f &&
                   std::isinf(plain->transmission.attenuationDistance) && !plain->clearcoat.texture &&
                   !plain->sheen.colorTexture && !plain->specular.texture && !plain->transmission.texture,
               "materials without extensions keep the neutral defaults");
        expect(!plain->translucent(), "an opaque material without transmission is not translucent");
        expect(materialLobeFlags(*plain) == 0u && materialLobeFlags(defaults) == 0u,
               "materials without extensions enable no lobes");
    }

    if (layered) {
        // A leased material keeps its extension textures through forced collection.
        const MaterialHandle handle =
            assets.model(loaded.handle)
                ? assets.mesh(assets.model(loaded.handle)->nodes[0].mesh)->primitives[0].material
                : MaterialHandle{};
        const AssetLease<MaterialHandle> lease = assets.lease(handle);
        const MaterialAsset extensionTextures = *layered;
        assets.collectUnused(true);
        bool kept = static_cast<bool>(lease);
        for (TextureHandle texture : extensionTextures.textures()) {
            kept &= !texture || assets.texture(texture) != nullptr;
        }
        expect(kept, "a material lease keeps every extension texture resident");
    }

    const ModelTextureSourcesResult sources = collectModelTextureSources(path);
    expect(static_cast<bool>(sources), "texture source collection accepts extension textures");
    std::filesystem::remove_all(root, error);
}

void testRecordPacking() {
    const MaterialAsset defaults;
    const MaterialRecord neutral = packMaterialRecord(defaults, {});
    expect(neutral.surface.z == 1.5f && neutral.surface.w == 0.0f && neutral.extraTextures.y == 0u &&
               neutral.specular == glm::vec4(1.0f) && neutral.clearcoat.x == 0.0f && neutral.sheen == glm::vec4(0.0f) &&
               neutral.volume == glm::vec4(0.0f),
           "the neutral record keeps the metallic-roughness model");
    expect(offsetof(MaterialRecord, surface) == 48 && offsetof(MaterialRecord, extraTextures) == 80 &&
               offsetof(MaterialRecord, clearcoat) == 96 && offsetof(MaterialRecord, volume) == 144 &&
               offsetof(MaterialRecord, layerTextures) == 176,
           "record members sit at their std430 offsets");

    MaterialAsset layered;
    layered.ior = 0.5f;
    layered.clearcoat.factor = 2.0f;
    layered.clearcoat.roughness = 0.2f;
    layered.sheen.color = {0.0f, 0.3f, 0.0f};
    layered.sheen.roughness = 0.4f;
    layered.specular.factor = 1.5f;
    layered.specular.colorTexture = TextureHandle{0, 1};
    layered.transmission.factor = 1.0f;
    layered.transmission.thickness = 0.5f;
    layered.transmission.attenuationColor = {0.5f, 1.0f, 0.25f};
    layered.transmission.attenuationDistance = 2.0f;
    const MaterialTextureSlots slots{.baseColor = 1,
                                     .normal = 2,
                                     .metallicRoughness = 3,
                                     .occlusion = 4,
                                     .emissive = 5,
                                     .clearcoat = 6,
                                     .clearcoatRoughness = 7,
                                     .clearcoatNormal = 8,
                                     .sheenColor = 9,
                                     .sheenRoughness = 10,
                                     .specular = 11,
                                     .specularColor = 12,
                                     .transmission = 13};
    const MaterialRecord record = packMaterialRecord(layered, slots);
    const uint32_t expectedFlags = material_lobes::clearcoat | material_lobes::sheen | material_lobes::transmission |
                                   material_lobes::specularTextures | material_lobes::volume;
    expect(record.extraTextures.y == expectedFlags, "every configured lobe sets its flag");
    expect(record.surface.z == 1.0f && record.clearcoat.x == 1.0f && record.specular.w == 1.0f,
           "out-of-range ior, clearcoat and specular factors are clamped");
    expect(near(record.volume.r, std::log(2.0f) / 2.0f) && near(record.volume.g, 0.0f) &&
               near(record.volume.b, std::log(4.0f) / 2.0f) && record.volume.w == 0.5f,
           "volume attenuation packs as a per-unit coefficient with the thickness");
    expect(record.textures == glm::uvec4(1, 2, 3, 4) && record.extraTextures.x == 5u && record.extraTextures.z == 13u &&
               record.lobeTextures == glm::uvec4(6, 7, 8, 9) && record.layerTextures == glm::uvec4(10, 11, 12, 0),
           "texture slots land in their record lanes");

    MaterialAsset thin = layered;
    thin.transmission.thickness = 0.0f;
    thin.transmission.attenuationDistance = std::numeric_limits<float>::infinity();
    const MaterialRecord thinRecord = packMaterialRecord(thin, slots);
    expect((thinRecord.extraTextures.y & material_lobes::volume) == 0u && thinRecord.volume == glm::vec4(0.0f),
           "thin-walled transmission has no volume");
}

void testSheenAlbedo() {
    using environment_lighting::integrateSheenAlbedo;
    bool bounded = true;
    for (float roughness : {0.05f, 0.3f, 0.6f, 1.0f}) {
        for (float nDotV : {0.05f, 0.3f, 0.7f, 1.0f}) {
            const float albedo = integrateSheenAlbedo(nDotV, roughness);
            bounded &= std::isfinite(albedo) && albedo >= 0.0f && albedo <= 1.0f;
        }
    }
    expect(bounded, "sheen albedo stays within [0, 1]");
    expect(integrateSheenAlbedo(0.1f, 0.5f) > integrateSheenAlbedo(1.0f, 0.5f),
           "sheen reflects most at grazing angles");
    expect(integrateSheenAlbedo(1.0f, 1.0f) > integrateSheenAlbedo(1.0f, 0.2f),
           "rough sheen reflects more head-on than smooth sheen");
    // Reference from a dense (64 x 128) integration of the same lobe.
    expect(near(integrateSheenAlbedo(1.0f, 1.0f), 0.164f, 0.01f), "white rough sheen head-on reflects about 16%");
}

void testGallery() {
    const std::filesystem::path path =
        std::filesystem::path(ZENITH_ASSET_DIR) / "showcase/materials/material_gallery.gltf";
    AssetManager assets;
    const AssetLoadResult<ModelHandle> loaded = assets.loadModel(path);
    expect(static_cast<bool>(loaded), "the material gallery loads: " + loaded.error);
    const ModelAsset* model = assets.model(loaded.handle);
    const MeshAsset* mesh = model && !model->nodes.empty() ? assets.mesh(model->nodes[0].mesh) : nullptr;
    expect(mesh && mesh->primitives.size() == 16, "the gallery has fifteen spheres and a backdrop");
    if (!mesh) {
        return;
    }
    uint32_t seen = 0;
    size_t transmissive = 0;
    for (const MeshPrimitive& primitive : mesh->primitives) {
        if (const MaterialAsset* material = assets.material(primitive.material)) {
            seen |= materialLobeFlags(*material);
            transmissive += material->transmission.factor > 0.0f ? 1u : 0u;
        }
    }
    expect((seen & material_lobes::clearcoat) && (seen & material_lobes::sheen) &&
               (seen & material_lobes::transmission) && (seen & material_lobes::volume),
           "the gallery shows clearcoat, sheen, transmission and volume");
    expect(transmissive == 3, "the gallery has clear, frosted and tinted glass");
    const MaterialAsset* noSpecular = assets.material(mesh->primitives[8].material);
    expect(noSpecular && noSpecular->specular.factor == 0.0f, "one sphere disables specular reflection");
    bool iorLadder = true;
    float previousIor = 0.0f;
    for (size_t index = 9; index < 15; ++index) {
        const MaterialAsset* material = assets.material(mesh->primitives[index].material);
        iorLadder &= material && material->ior > previousIor;
        previousIor = material ? material->ior : previousIor;
    }
    expect(iorLadder && near(previousIor, 3.0f), "the back row ends in an increasing ior ladder");
}
} // namespace

int main() {
    testImport();
    testRecordPacking();
    testSheenAlbedo();
    testGallery();
    if (failures > 0) {
        std::cerr << failures << " material extension test(s) failed\n";
        return 1;
    }
    std::cout << "Material extension tests passed\n";
    return 0;
}
