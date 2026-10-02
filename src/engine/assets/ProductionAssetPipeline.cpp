#include "engine/assets/ProductionAssetPipeline.hpp"

#include "engine/assets/ContentHash.hpp"

#include <array>
#include <fstream>

namespace {
std::string codecName(CompressedMeshCodec codec) {
    return codec == CompressedMeshCodec::Meshopt ? "Meshopt" : "Draco";
}
} // namespace

AssetPipelineResult validateAssetLimits(const AssetValidationCounts& counts, const AssetImportLimits& limits) {
    if (counts.sourceBytes > limits.maximumSourceBytes) {
        return {.error = "source byte limit exceeded"};
    }
    if (counts.decodedBytes > limits.maximumDecodedBytes) {
        return {.error = "decoded byte limit exceeded"};
    }
    if (counts.vertices > limits.maximumVertices) {
        return {.error = "vertex limit exceeded"};
    }
    if (counts.indices > limits.maximumIndices) {
        return {.error = "index limit exceeded"};
    }
    if (counts.morphTargets > limits.maximumMorphTargets) {
        return {.error = "morph-target limit exceeded"};
    }
    if (counts.textureWidth > limits.maximumTextureDimension || counts.textureHeight > limits.maximumTextureDimension) {
        return {.error = "texture dimension limit exceeded"};
    }
    return {.success = true};
}

void CompressedMeshDecoderRegistry::registerDecoder(const CompressedMeshDecoder& decoder) {
    for (const CompressedMeshDecoder*& existing : m_decoders) {
        if (existing->codec() == decoder.codec()) {
            existing = &decoder;
            return;
        }
    }
    m_decoders.push_back(&decoder);
}

AssetPipelineResult CompressedMeshDecoderRegistry::decode(CompressedMeshCodec codec, std::span<const uint8_t> input,
                                                          std::vector<uint8_t>& output,
                                                          uint64_t maximumOutputBytes) const {
    for (const CompressedMeshDecoder* decoder : m_decoders) {
        if (decoder->codec() == codec) {
            output.clear();
            const AssetPipelineResult result = decoder->decode(input, output, maximumOutputBytes);
            if (!result) {
                output.clear();
            }
            if (output.size() > maximumOutputBytes) {
                output.clear();
                return {.error = codecName(codec) + " decoder exceeded its output limit"};
            }
            return result;
        }
    }
    return {.error = codecName(codec) + " compressed input requires an optional decoder that is not installed"};
}

DerivedTextureFormat selectKtx2TargetFormat(const TextureFormatCapabilities& capabilities) {
    if (capabilities.bc7) {
        return DerivedTextureFormat::Bc7;
    }
    if (capabilities.astc4x4) {
        return DerivedTextureFormat::Astc4x4;
    }
    if (capabilities.etc2) {
        return DerivedTextureFormat::Etc2;
    }
    return DerivedTextureFormat::Rgba8;
}

AssetPipelineResult validateKtx2(std::span<const uint8_t> bytes, const AssetImportLimits& limits) {
    constexpr std::array<uint8_t, 12> identifier{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 80 || !std::equal(identifier.begin(), identifier.end(), bytes.begin())) {
        return {.error = "invalid or truncated KTX2 header"};
    }
    auto read32 = [&bytes](size_t offset) {
        return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 16u) | (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
    };
    const uint32_t width = read32(20);
    const uint32_t height = read32(24);
    if (width == 0 || height == 0) {
        return {.error = "KTX2 has an empty base level"};
    }
    return validateAssetLimits({.sourceBytes = bytes.size(),
                                .decodedBytes = uint64_t(width) * height * 4,
                                .textureWidth = width,
                                .textureHeight = height},
                               limits);
}

DerivedAssetCache::DerivedAssetCache(std::filesystem::path root)
    : m_root(std::move(root)) {}

AssetPipelineResult DerivedAssetCache::key(const DerivedAssetRequest& request, std::string& output) const {
    ContentHasher hasher;
    if (!hasher.appendFile(request.source)) {
        return {.error = "could not open derived-asset source: " + request.source.string()};
    }
    // The fields are appended without separators. Existing cache entries are
    // named by this exact byte sequence, so changing it invalidates them.
    hasher.append(request.importSettings).append(request.toolVersion).append(request.targetPlatform);
    output = hasher.hex();
    return {.success = true};
}

AssetPipelineResult DerivedAssetCache::store(const DerivedAssetRequest& request, std::span<const uint8_t> bytes,
                                             DerivedAssetRecord& record) {
    std::string cacheKey;
    if (const AssetPipelineResult result = key(request, cacheKey); !result) {
        return result;
    }
    std::error_code error;
    std::filesystem::create_directories(m_root, error);
    if (error) {
        return {.error = "could not create derived-data directory: " + error.message()};
    }
    const std::filesystem::path destination = m_root / (cacheKey + ".zderived");
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        return {.error = "could not write derived asset: " + temporary.string()};
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(temporary, destination, error);
    }
    if (error) {
        return {.error = "could not commit derived asset: " + error.message()};
    }
    record = {cacheKey, destination, bytes.size()};
    return {.success = true};
}

AssetPipelineResult DerivedAssetCache::inspect(std::string_view keyValue, DerivedAssetRecord& record) const {
    if (keyValue.empty() || keyValue.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        return {.error = "invalid derived-asset key"};
    }
    const std::filesystem::path path = m_root / (std::string(keyValue) + ".zderived");
    std::error_code error;
    const uint64_t bytes = std::filesystem::file_size(path, error);
    if (error) {
        return {.error = "derived asset is missing: " + path.string()};
    }
    record = {std::string(keyValue), path, bytes};
    return {.success = true};
}

AssetPipelineResult DerivedAssetCache::invalidate(std::string_view keyValue) {
    DerivedAssetRecord record;
    if (const AssetPipelineResult result = inspect(keyValue, record); !result) {
        return result;
    }
    std::error_code error;
    std::filesystem::remove(record.path, error);
    return error ? AssetPipelineResult{.error = "could not invalidate derived asset: " + error.message()}
                 : AssetPipelineResult{.success = true};
}

const std::filesystem::path& DerivedAssetCache::root() const {
    return m_root;
}
