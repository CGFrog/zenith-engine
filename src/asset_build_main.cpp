#include "engine/assets/CompressedTexture.hpp"
#include "engine/assets/ImageDecoder.hpp"
#include "engine/assets/ModelImporter.hpp"
#include "engine/assets/ProductionAssetPipeline.hpp"
#include "engine/assets/TextureCooker.hpp"

#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>

namespace {
void printUsage() {
    std::cerr << "usage:\n"
                 "  zenith_asset_build <source> <derived-root> <settings> <tool-version> <platform>\n"
                 "  zenith_asset_build cook-texture <image> <derived-root> <color|normal|data>\n"
                 "  zenith_asset_build cook-model-textures <model.gltf> <derived-root>\n";
}

std::optional<TextureUsage> parseUsage(std::string_view name) {
    if (name == "color") {
        return TextureUsage::Color;
    }
    if (name == "normal") {
        return TextureUsage::Normal;
    }
    if (name == "data") {
        return TextureUsage::Data;
    }
    return std::nullopt;
}

int storeDerivedCopy(char** argv) {
    const DerivedAssetRequest request{argv[1], argv[3], argv[4], argv[5]};
    std::ifstream input(request.source, std::ios::binary);
    if (!input) {
        std::cerr << "could not open source asset: " << request.source << '\n';
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    DerivedAssetCache cache(argv[2]);
    DerivedAssetRecord record;
    const AssetPipelineResult result = cache.store(request, bytes, record);
    if (!result) {
        std::cerr << result.error << '\n';
        return 1;
    }
    std::cout << record.key << ' ' << record.path.generic_string() << ' ' << record.bytes << '\n';
    return 0;
}

// Cooks one source image into the derived cache unless an identical cook
// already exists. Returns false after reporting an error.
bool cookTexture(const std::filesystem::path& source, DerivedAssetCache& cache, TextureUsage usage) {
    const DerivedAssetRequest request = cookedTextureRequest(source, usage);
    std::string key;
    DerivedAssetRecord record;
    if (const AssetPipelineResult keyed = cache.key(request, key); !keyed) {
        std::cerr << keyed.error << '\n';
        return false;
    }
    if (cache.inspect(key, record)) {
        std::cout << record.key << ' ' << record.path.generic_string() << " cached\n";
        return true;
    }
    const ImageDecodeResult decoded = decodeImageFile(source);
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return false;
    }
    const TextureCookResult cooked = cookBlockCompressedTexture(decoded.texture, usage);
    if (!cooked) {
        std::cerr << "Cannot cook " << source.string() << ": " << cooked.error << '\n';
        return false;
    }
    const std::vector<uint8_t> ktx2 = writeBlockCompressedKtx2(cooked.texture);
    if (const AssetPipelineResult stored = cache.store(request, ktx2, record); !stored) {
        std::cerr << stored.error << '\n';
        return false;
    }
    std::cout << record.key << ' ' << record.path.generic_string() << ' '
              << (cooked.texture.format == TextureFormat::Bc5 ? "bc5" : "bc7") << ' ' << record.bytes << '\n';
    return true;
}

int cookModelTextures(const std::filesystem::path& model, const std::filesystem::path& derivedRoot) {
    const ModelTextureSourcesResult sources = collectModelTextureSources(model);
    if (!sources) {
        std::cerr << sources.error << '\n';
        return 1;
    }
    DerivedAssetCache cache(derivedRoot);
    bool success = true;
    for (const ModelTextureSource& source : sources.sources) {
        success = cookTexture(source.path, cache, source.usage) && success;
    }
    return success ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command == "cook-texture" && argc == 5) {
        const std::optional<TextureUsage> usage = parseUsage(argv[4]);
        if (!usage) {
            printUsage();
            return 2;
        }
        DerivedAssetCache cache(argv[3]);
        return cookTexture(argv[2], cache, *usage) ? 0 : 1;
    }
    if (command == "cook-model-textures" && argc == 4) {
        return cookModelTextures(argv[2], argv[3]);
    }
    if (argc == 6) {
        return storeDerivedCopy(argv);
    }
    printUsage();
    return 2;
}
