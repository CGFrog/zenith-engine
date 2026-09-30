#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct AssetImportLimits {
    uint64_t maximumSourceBytes{2ull * 1024ull * 1024ull * 1024ull};
    uint64_t maximumDecodedBytes{1024ull * 1024ull * 1024ull};
    uint32_t maximumVertices{4'000'000};
    uint32_t maximumIndices{12'000'000};
    uint32_t maximumMorphTargets{64};
    uint32_t maximumTextureDimension{16'384};
};

struct AssetValidationCounts {
    uint64_t sourceBytes{0};
    uint64_t decodedBytes{0};
    uint64_t vertices{0};
    uint64_t indices{0};
    uint32_t morphTargets{0};
    uint32_t textureWidth{0};
    uint32_t textureHeight{0};
};

struct AssetPipelineResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

AssetPipelineResult validateAssetLimits(const AssetValidationCounts& counts, const AssetImportLimits& limits = {});

enum class CompressedMeshCodec : uint8_t {
    Meshopt,
    Draco
};

class CompressedMeshDecoder {
  public:
    virtual ~CompressedMeshDecoder() = default;
    virtual CompressedMeshCodec codec() const = 0;
    virtual AssetPipelineResult decode(std::span<const uint8_t> input, std::vector<uint8_t>& output,
                                       uint64_t maximumOutputBytes) const = 0;
};

class CompressedMeshDecoderRegistry {
  public:
    void registerDecoder(const CompressedMeshDecoder& decoder);
    AssetPipelineResult decode(CompressedMeshCodec codec, std::span<const uint8_t> input, std::vector<uint8_t>& output,
                               uint64_t maximumOutputBytes) const;

  private:
    std::vector<const CompressedMeshDecoder*> m_decoders;
};

enum class DerivedTextureFormat : uint8_t {
    Rgba8,
    Bc7,
    Astc4x4,
    Etc2
};

struct TextureFormatCapabilities {
    bool bc7{false};
    bool astc4x4{false};
    bool etc2{false};
};

DerivedTextureFormat selectKtx2TargetFormat(const TextureFormatCapabilities& capabilities);
AssetPipelineResult validateKtx2(std::span<const uint8_t> bytes, const AssetImportLimits& limits = {});

struct DerivedAssetRequest {
    std::filesystem::path source;
    std::string importSettings;
    std::string toolVersion;
    std::string targetPlatform;
};

struct DerivedAssetRecord {
    std::string key;
    std::filesystem::path path;
    uint64_t bytes{0};
};

class DerivedAssetCache {
  public:
    explicit DerivedAssetCache(std::filesystem::path root);

    AssetPipelineResult key(const DerivedAssetRequest& request, std::string& output) const;
    AssetPipelineResult store(const DerivedAssetRequest& request, std::span<const uint8_t> bytes,
                              DerivedAssetRecord& record);
    AssetPipelineResult inspect(std::string_view key, DerivedAssetRecord& record) const;
    AssetPipelineResult invalidate(std::string_view key);
    const std::filesystem::path& root() const;

  private:
    std::filesystem::path m_root;
};
