#pragma once

#include "Assets.hpp"
#include "ProductionAssetPipeline.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Block-compressed texture containers and the cooked-texture convention shared
// by the offline cooker (zenith_asset_build) and runtime import.
//
// Cooked textures live in a DerivedAssetCache root. The cache key covers the
// source image bytes, cookedTextureRequest()'s settings string (usage, format,
// and mip policy), the cooker version, and the "desktop-bc" target, so editing
// a source image or changing the cook policy simply misses the cache and the
// importer falls back to decoding the source.

// Vulkan format identifiers stored in KTX2 headers.
inline constexpr uint32_t ktx2FormatBc5Unorm = 141;
inline constexpr uint32_t ktx2FormatBc7Unorm = 145;
inline constexpr uint32_t ktx2FormatBc7Srgb = 146;

// Number of levels in a full mip chain down to 1x1.
uint32_t fullMipLevelCount(uint32_t width, uint32_t height);

// Bytes of one block-compressed level (16 bytes per 4x4 texel block).
uint64_t blockCompressedLevelBytes(uint32_t width, uint32_t height);

// The cooked format for a usage: BC5 for normals, BC7 for color and data.
TextureFormat cookedTextureFormat(TextureUsage usage);
bool textureFormatSupported(TextureFormat format, const TextureFormatCapabilities& capabilities);

// Checks that a block-compressed asset's mip table is complete, contiguous,
// and sized for its format and extent.
AssetPipelineResult validateBlockCompressedTexture(const TextureAsset& texture);

// Serializes a validated block-compressed texture as an uncompressed KTX2
// container with a basic data-format descriptor. Returns an empty vector for
// RGBA8 or invalid input.
std::vector<uint8_t> writeBlockCompressedKtx2(const TextureAsset& texture);

struct BlockCompressedDecodeResult {
    TextureAsset texture;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Reads a BC5/BC7 KTX2 container (all stored mip levels). The caller has
// already checked the identifier and limits with validateKtx2().
BlockCompressedDecodeResult readBlockCompressedKtx2(std::span<const uint8_t> bytes, std::string_view label);
bool isBlockCompressedKtx2Format(uint32_t vkFormat);

inline constexpr std::string_view cookedTextureToolVersion = "zenith-texture-cook-1";
inline constexpr std::string_view cookedTexturePlatform = "desktop-bc";

DerivedAssetRequest cookedTextureRequest(const std::filesystem::path& source, TextureUsage usage);

struct CookedTextureLookup {
    // Set when a valid cooked texture exists for the source and usage.
    std::optional<TextureAsset> texture;
    // Set when a cooked entry exists but cannot be used. Missing entries are
    // not errors; the caller decodes the source instead.
    std::string error;
};

CookedTextureLookup loadCookedTexture(const std::filesystem::path& derivedRoot, const std::filesystem::path& source,
                                      TextureUsage usage);
