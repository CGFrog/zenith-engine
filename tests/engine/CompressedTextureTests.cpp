#include "engine/assets/Assets.hpp"
#include "engine/assets/CompressedTexture.hpp"
#include "engine/assets/ImageDecoder.hpp"
#include "engine/assets/ModelImporter.hpp"
#include "engine/assets/ProductionAssetPipeline.hpp"
#include "engine/assets/TextureCooker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

TextureAsset gradientTexture(uint32_t width, uint32_t height) {
    TextureAsset texture{.width = width, .height = height};
    texture.rgba.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* texel = &texture.rgba[(size_t(y) * width + x) * 4];
            texel[0] = static_cast<uint8_t>(x * 255 / std::max(width - 1, 1u));
            texel[1] = static_cast<uint8_t>(y * 255 / std::max(height - 1, 1u));
            texel[2] = static_cast<uint8_t>(128 + (x + y) % 32);
            texel[3] = 255;
        }
    }
    return texture;
}

// A smooth bump encoded as a tangent-space RGB normal map.
TextureAsset bumpNormalTexture(uint32_t extent) {
    TextureAsset texture{.width = extent, .height = extent};
    texture.rgba.resize(size_t(extent) * extent * 4);
    for (uint32_t y = 0; y < extent; ++y) {
        for (uint32_t x = 0; x < extent; ++x) {
            const float slopeX = 0.5f * std::sin(float(x) * 0.2f);
            const float slopeY = 0.5f * std::cos(float(y) * 0.15f);
            const float length = std::sqrt(slopeX * slopeX + slopeY * slopeY + 1.0f);
            uint8_t* texel = &texture.rgba[(size_t(y) * extent + x) * 4];
            texel[0] = static_cast<uint8_t>(std::lround((slopeX / length * 0.5f + 0.5f) * 255.0f));
            texel[1] = static_cast<uint8_t>(std::lround((slopeY / length * 0.5f + 0.5f) * 255.0f));
            texel[2] = static_cast<uint8_t>(std::lround((1.0f / length * 0.5f + 0.5f) * 255.0f));
            texel[3] = 255;
        }
    }
    return texture;
}

int maximumChannelError(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual, uint32_t channels) {
    if (expected.size() != actual.size()) {
        return 256;
    }
    int worst = 0;
    for (size_t index = 0; index < expected.size(); ++index) {
        if (index % 4 < channels) {
            worst = std::max(worst, std::abs(int(expected[index]) - int(actual[index])));
        }
    }
    return worst;
}

std::vector<uint8_t> rgbaKtx2(const TextureAsset& texture) {
    std::vector<uint8_t> bytes(104 + texture.rgba.size(), 0);
    const std::array<uint8_t, 12> identifier{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    std::copy(identifier.begin(), identifier.end(), bytes.begin());
    const auto write32 = [&bytes](size_t offset, uint32_t value) {
        for (uint32_t byte = 0; byte < 4; ++byte) {
            bytes[offset + byte] = uint8_t(value >> (byte * 8));
        }
    };
    write32(12, 37);
    write32(20, texture.width);
    write32(24, texture.height);
    write32(40, 1);
    write32(80, 104);
    write32(88, static_cast<uint32_t>(texture.rgba.size()));
    std::copy(texture.rgba.begin(), texture.rgba.end(), bytes.begin() + 104);
    return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void testMipChainLayout() {
    expect(fullMipLevelCount(1, 1) == 1 && fullMipLevelCount(1024, 1024) == 11 && fullMipLevelCount(37, 21) == 6,
           "full mip chains run down to 1x1");
    expect(blockCompressedLevelBytes(1, 1) == 16 && blockCompressedLevelBytes(5, 4) == 32 &&
               blockCompressedLevelBytes(1024, 1024) == 1024 * 1024,
           "block-compressed levels round up to whole 4x4 blocks of 16 bytes");
    expect(cookedTextureFormat(TextureUsage::Normal) == TextureFormat::Bc5 &&
               cookedTextureFormat(TextureUsage::Color) == TextureFormat::Bc7 &&
               cookedTextureFormat(TextureUsage::Data) == TextureFormat::Bc7,
           "normals cook to BC5; color and data cook to BC7");
    expect(!textureFormatSupported(TextureFormat::Bc7, {}) &&
               textureFormatSupported(TextureFormat::Bc5, {.bc5 = true}) &&
               textureFormatSupported(TextureFormat::Rgba8, {}),
           "format support follows renderer capabilities and RGBA8 is always available");
}

void testColorCookRoundTrip() {
    const TextureAsset source = gradientTexture(37, 21);
    const TextureCookResult cooked = cookBlockCompressedTexture(source, TextureUsage::Color);
    expect(static_cast<bool>(cooked), "a color texture cooks to BC7");
    expect(cooked.texture.format == TextureFormat::Bc7 && cooked.texture.usage == TextureUsage::Color &&
               cooked.texture.rgba.empty() && cooked.texture.mips.size() == 6,
           "cooked color textures carry a full BC7 mip chain and no RGBA8 copy");
    expect(static_cast<bool>(validateBlockCompressedTexture(cooked.texture)), "cooked mip tables are consistent");
    expect(cooked.texture.mips.back().width == 1 && cooked.texture.mips.back().height == 1 &&
               cooked.texture.mips[1].width == 18 && cooked.texture.mips[1].height == 10,
           "mip extents halve and clamp to one texel");
    const std::vector<uint8_t> decoded = decodeBlockCompressedLevel(cooked.texture, 0);
    expect(maximumChannelError(source.rgba, decoded, 4) <= 24, "BC7 level 0 stays close to the source gradient");
    const TextureCookResult again = cookBlockCompressedTexture(source, TextureUsage::Color);
    expect(again.texture.compressed == cooked.texture.compressed, "cooking is deterministic");

    TextureAsset compressedInput = cooked.texture;
    expect(!cookBlockCompressedTexture(compressedInput, TextureUsage::Color), "cooking rejects compressed input");
    expect(!cookBlockCompressedTexture(TextureAsset{.width = 4, .height = 4}, TextureUsage::Color),
           "cooking rejects textures without RGBA8 texels");
}

void testNormalCookRoundTrip() {
    const TextureAsset source = bumpNormalTexture(64);
    const TextureCookResult cooked = cookBlockCompressedTexture(source, TextureUsage::Normal);
    expect(cooked && cooked.texture.format == TextureFormat::Bc5 && cooked.texture.mips.size() == 7,
           "a normal map cooks to a full BC5 mip chain");
    const std::vector<uint8_t> decoded = decodeBlockCompressedLevel(cooked.texture, 0);
    expect(maximumChannelError(source.rgba, decoded, 2) <= 4, "BC5 keeps normal X and Y within four steps");
    expect(decoded.size() == source.rgba.size() && decoded[2] == 0 && decoded[3] == 255,
           "BC5 decodes to two channels; shaders reconstruct Z");
    const std::vector<uint8_t> smallest = decodeBlockCompressedLevel(cooked.texture, 6);
    const float x = smallest[0] / 255.0f * 2.0f - 1.0f;
    const float y = smallest[1] / 255.0f * 2.0f - 1.0f;
    expect(smallest.size() == 4 && x * x + y * y < 1.0f, "renormalized normal mips keep a valid reconstructed Z");
}

void testKtx2RoundTrip() {
    const TextureCookResult color = cookBlockCompressedTexture(gradientTexture(16, 8), TextureUsage::Color);
    const std::vector<uint8_t> colorFile = writeBlockCompressedKtx2(color.texture);
    expect(static_cast<bool>(validateKtx2(colorFile)), "written BC7 KTX2 passes container validation");
    const ImageDecodeResult decoded = decodeImageMemory(colorFile.data(), colorFile.size(), "BC7 fixture");
    expect(decoded && decoded.texture.format == TextureFormat::Bc7 && decoded.texture.usage == TextureUsage::Color &&
               decoded.texture.width == 16 && decoded.texture.height == 8 && decoded.texture.mips.size() == 5 &&
               decoded.texture.compressed == color.texture.compressed,
           "BC7 KTX2 reads back every mip level unchanged");
    expect(colorFile[12] == ktx2FormatBc7Srgb, "color BC7 records the sRGB format");

    const TextureCookResult data = cookBlockCompressedTexture(gradientTexture(8, 8), TextureUsage::Data);
    const std::vector<uint8_t> dataFile = writeBlockCompressedKtx2(data.texture);
    expect(dataFile[12] == ktx2FormatBc7Unorm, "data BC7 records the linear format");

    const TextureCookResult normal = cookBlockCompressedTexture(bumpNormalTexture(8), TextureUsage::Normal);
    const std::vector<uint8_t> normalFile = writeBlockCompressedKtx2(normal.texture);
    const ImageDecodeResult normalDecoded = decodeImageMemory(normalFile.data(), normalFile.size(), "BC5 fixture");
    expect(normalFile[12] == ktx2FormatBc5Unorm && normalDecoded &&
               normalDecoded.texture.format == TextureFormat::Bc5 &&
               normalDecoded.texture.usage == TextureUsage::Normal &&
               normalDecoded.texture.compressed == normal.texture.compressed,
           "BC5 KTX2 round-trips as a normal texture");

    std::vector<uint8_t> truncated = colorFile;
    truncated.resize(truncated.size() - 8);
    expect(!decodeImageMemory(truncated.data(), truncated.size(), "truncated BC7"),
           "truncated block-compressed KTX2 is rejected");
    std::vector<uint8_t> missized = colorFile;
    missized[88] ^= 0x10;
    expect(!decodeImageMemory(missized.data(), missized.size(), "mis-sized BC7"),
           "mis-sized block-compressed levels are rejected");
    expect(writeBlockCompressedKtx2(gradientTexture(4, 4)).empty(), "RGBA8 textures are not written as BC KTX2");
}

std::string modelWithTextures() {
    return R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [{ "nodes": [0] }],
  "nodes": [{ "mesh": 0 }],
  "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 }] }],
  "materials": [{
    "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 }, "metallicRoughnessTexture": { "index": 2 } },
    "normalTexture": { "index": 1 }
  }],
  "images": [{ "uri": "base.ktx2" }, { "uri": "normal.ktx2" }, { "uri": "orm.ktx2" }],
  "textures": [{ "source": 0 }, { "source": 1 }, { "source": 2 }],
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

struct ImportedTextures {
    const TextureAsset* baseColor{nullptr};
    const TextureAsset* normal{nullptr};
    const TextureAsset* orm{nullptr};
};

ImportedTextures importTextures(AssetManager& assets, const std::filesystem::path& model) {
    const AssetLoadResult<ModelHandle> loaded = assets.loadModel(model);
    const ModelAsset* asset = assets.model(loaded.handle);
    const MeshAsset* mesh = asset && !asset->nodes.empty() ? assets.mesh(asset->nodes[0].mesh) : nullptr;
    const MaterialAsset* material =
        mesh && !mesh->primitives.empty() ? assets.material(mesh->primitives[0].material) : nullptr;
    if (!material) {
        return {};
    }
    return {assets.texture(material->baseColorTexture), assets.texture(material->normalTexture),
            assets.texture(material->metallicRoughnessTexture)};
}

void testCookedTextureImport() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith-compressed-texture-tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const std::filesystem::path derived = root / "derived";
    const std::filesystem::path model = root / "model.gltf";
    writeFile(root / "base.ktx2", rgbaKtx2(gradientTexture(8, 8)));
    writeFile(root / "normal.ktx2", rgbaKtx2(bumpNormalTexture(8)));
    writeFile(root / "orm.ktx2", rgbaKtx2(gradientTexture(8, 4)));
    writeText(model, modelWithTextures());

    const ModelTextureSourcesResult sources = collectModelTextureSources(model);
    expect(sources && sources.sources.size() == 3 && sources.sources[0].usage == TextureUsage::Color &&
               sources.sources[1].usage == TextureUsage::Normal && sources.sources[2].usage == TextureUsage::Data,
           "material slots give external images color, normal, and data usage");

    {
        AssetManager assets;
        const ImportedTextures textures = importTextures(assets, model);
        expect(textures.baseColor && textures.normal && textures.orm &&
                   textures.baseColor->format == TextureFormat::Rgba8 &&
                   textures.normal->usage == TextureUsage::Normal && textures.orm->usage == TextureUsage::Data,
               "without a cooked root, imports keep RGBA8 and record slot usage");
    }
    {
        AssetManager assets;
        assets.setCookedTextureRoot(derived);
        assets.setTextureFormatCapabilities({.bc7 = true, .bc5 = true});
        const ImportedTextures textures = importTextures(assets, model);
        expect(textures.baseColor && textures.baseColor->format == TextureFormat::Rgba8,
               "a missing cooked entry falls back to the source image");
    }

    DerivedAssetCache cache(derived);
    for (const ModelTextureSource& source : sources.sources) {
        const ImageDecodeResult decoded = decodeImageFile(source.path);
        const TextureCookResult cooked = cookBlockCompressedTexture(decoded.texture, source.usage);
        DerivedAssetRecord record;
        expect(cooked && cache.store(cookedTextureRequest(source.path, source.usage),
                                     writeBlockCompressedKtx2(cooked.texture), record),
               "cooked textures are stored in the derived cache");
    }
    {
        AssetManager assets;
        assets.setCookedTextureRoot(derived);
        assets.setTextureFormatCapabilities({.bc7 = true, .bc5 = true});
        const ImportedTextures textures = importTextures(assets, model);
        expect(textures.baseColor && textures.baseColor->format == TextureFormat::Bc7 &&
                   textures.baseColor->usage == TextureUsage::Color && textures.baseColor->mips.size() == 4,
               "cooked BC7 color textures replace the source image");
        expect(textures.normal && textures.normal->format == TextureFormat::Bc5 &&
                   textures.normal->usage == TextureUsage::Normal,
               "cooked BC5 normal maps replace the source image");
        expect(textures.orm && textures.orm->format == TextureFormat::Bc7 && textures.orm->usage == TextureUsage::Data,
               "cooked BC7 data textures replace the source image");
    }
    {
        AssetManager assets;
        assets.setCookedTextureRoot(derived);
        assets.setTextureFormatCapabilities({.bc7 = true});
        const ImportedTextures textures = importTextures(assets, model);
        expect(textures.baseColor && textures.baseColor->format == TextureFormat::Bc7 && textures.normal &&
                   textures.normal->format == TextureFormat::Rgba8,
               "each usage falls back independently when its cooked format is unsupported");
    }
    {
        AssetManager assets;
        assets.setCookedTextureRoot(derived);
        const ImportedTextures textures = importTextures(assets, model);
        expect(textures.baseColor && textures.baseColor->format == TextureFormat::Rgba8,
               "devices without BC support keep the RGBA8 path");
    }
    {
        // The normal image also feeds emissive color, which BC5 cannot represent.
        std::string mixed = modelWithTextures();
        const std::string normalSlot = R"("normalTexture": { "index": 1 })";
        mixed.replace(mixed.find(normalSlot), normalSlot.size(), normalSlot + R"(, "emissiveTexture": { "index": 1 })");
        const std::filesystem::path mixedModel = root / "mixed.gltf";
        writeText(mixedModel, mixed);
        const ModelTextureSourcesResult mixedSources = collectModelTextureSources(mixedModel);
        expect(mixedSources && mixedSources.sources.size() == 2,
               "normal maps shared with color slots are not offered for cooking");
        AssetManager assets;
        assets.setCookedTextureRoot(derived);
        assets.setTextureFormatCapabilities({.bc7 = true, .bc5 = true});
        const ImportedTextures textures = importTextures(assets, mixedModel);
        expect(textures.normal && textures.normal->format == TextureFormat::Rgba8,
               "normal maps shared with color slots keep the RGBA8 source");
    }
    {
        const TextureCookResult cooked = cookBlockCompressedTexture(gradientTexture(8, 8), TextureUsage::Color);
        writeFile(root / "standalone-bc7.ktx2", writeBlockCompressedKtx2(cooked.texture));
        AssetManager assets;
        expect(static_cast<bool>(assets.loadTexture(root / "orm.ktx2")), "standalone RGBA8 KTX2 still loads");
        expect(!assets.loadTexture(root / "standalone-bc7.ktx2"),
               "standalone textures reject block-compressed data that CPU consumers cannot read");
    }

    std::string normalKey;
    DerivedAssetRecord normalRecord;
    expect(cache.key(cookedTextureRequest(root / "normal.ktx2", TextureUsage::Normal), normalKey) &&
               cache.inspect(normalKey, normalRecord),
           "cooked entries are addressed by source bytes and cook settings");
    writeText(normalRecord.path, "not a ktx2 file");
    {
        AssetManager assets;
        assets.setCookedTextureRoot(derived);
        assets.setTextureFormatCapabilities({.bc7 = true, .bc5 = true});
        const ImportedTextures textures = importTextures(assets, model);
        expect(textures.normal && textures.normal->format == TextureFormat::Rgba8,
               "a corrupt cooked entry falls back to the source image");
    }

    writeFile(root / "base.ktx2", rgbaKtx2(gradientTexture(4, 4)));
    {
        AssetManager assets;
        assets.setCookedTextureRoot(derived);
        assets.setTextureFormatCapabilities({.bc7 = true, .bc5 = true});
        const ImportedTextures textures = importTextures(assets, model);
        expect(textures.baseColor && textures.baseColor->format == TextureFormat::Rgba8 &&
                   textures.baseColor->width == 4,
               "editing a source image invalidates its cooked entry");
    }
    std::filesystem::remove_all(root, error);
}
} // namespace

int main() {
    testMipChainLayout();
    testColorCookRoundTrip();
    testNormalCookRoundTrip();
    testKtx2RoundTrip();
    testCookedTextureImport();
    if (failures != 0) {
        std::cerr << failures << " compressed texture test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Compressed texture tests passed\n";
    return EXIT_SUCCESS;
}
