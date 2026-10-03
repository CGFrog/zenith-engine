#include "engine/assets/CompressedTexture.hpp"
#include "engine/platform/FileContents.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace {
constexpr std::array<uint8_t, 12> ktx2Identifier{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr size_t ktx2HeaderBytes = 80;
constexpr size_t ktx2LevelIndexEntryBytes = 24;
constexpr uint64_t blockBytes = 16;

// Khronos Data Format constants used by the basic descriptor block.
constexpr uint32_t dfdModelBc5 = 131;
constexpr uint32_t dfdModelBc7 = 134;
constexpr uint32_t dfdPrimariesBt709 = 1;
constexpr uint32_t dfdTransferLinear = 1;
constexpr uint32_t dfdTransferSrgb = 2;

uint32_t read32(std::span<const uint8_t> bytes, size_t offset) {
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8u) | (uint32_t(bytes[offset + 2]) << 16u) |
           (uint32_t(bytes[offset + 3]) << 24u);
}

uint64_t read64(std::span<const uint8_t> bytes, size_t offset) {
    return uint64_t(read32(bytes, offset)) | (uint64_t(read32(bytes, offset + 4)) << 32u);
}

void write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (8u * index));
    }
}

void write64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    write32(bytes, offset, static_cast<uint32_t>(value));
    write32(bytes, offset + 4, static_cast<uint32_t>(value >> 32u));
}

size_t alignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

uint32_t ktx2Format(const TextureAsset& texture) {
    if (texture.format == TextureFormat::Bc5) {
        return ktx2FormatBc5Unorm;
    }
    return texture.usage == TextureUsage::Color ? ktx2FormatBc7Srgb : ktx2FormatBc7Unorm;
}

// Basic data-format descriptor: one sample for BC7, red and green for BC5.
std::vector<uint8_t> dataFormatDescriptor(const TextureAsset& texture) {
    const bool bc5 = texture.format == TextureFormat::Bc5;
    const uint32_t sampleCount = bc5 ? 2u : 1u;
    const uint32_t blockSize = 24u + 16u * sampleCount;
    std::vector<uint8_t> descriptor(4 + blockSize, 0);
    write32(descriptor, 0, static_cast<uint32_t>(descriptor.size()));
    write32(descriptor, 4, 0); // Khronos vendor, basic descriptor type.
    write32(descriptor, 8, 2u | (blockSize << 16u));
    const uint32_t model = bc5 ? dfdModelBc5 : dfdModelBc7;
    const uint32_t transfer = texture.format == TextureFormat::Bc7 && texture.usage == TextureUsage::Color
                                  ? dfdTransferSrgb
                                  : dfdTransferLinear;
    write32(descriptor, 12, model | (dfdPrimariesBt709 << 8u) | (transfer << 16u));
    write32(descriptor, 16, 3u | (3u << 8u)); // 4x4 texel blocks, stored as dimension - 1.
    write32(descriptor, 20, static_cast<uint32_t>(blockBytes));
    for (uint32_t sample = 0; sample < sampleCount; ++sample) {
        const size_t offset = 28 + 16 * sample;
        const uint32_t bitOffset = bc5 ? 64u * sample : 0u;
        const uint32_t bitLength = bc5 ? 63u : 127u; // Stored as length - 1.
        const uint32_t channel = sample;             // BC5 red/green, or BC7 color.
        write32(descriptor, offset, bitOffset | (bitLength << 16u) | (channel << 24u));
        write32(descriptor, offset + 12, 0xFFFFFFFFu);
    }
    return descriptor;
}

std::string_view usageName(TextureUsage usage) {
    switch (usage) {
    case TextureUsage::Normal:
        return "normal";
    case TextureUsage::Data:
        return "data";
    case TextureUsage::Color:
    default:
        return "color";
    }
}
} // namespace

uint32_t fullMipLevelCount(uint32_t width, uint32_t height) {
    return static_cast<uint32_t>(std::bit_width(std::max({width, height, 1u})));
}

uint64_t blockCompressedLevelBytes(uint32_t width, uint32_t height) {
    return uint64_t((width + 3) / 4) * ((height + 3) / 4) * blockBytes;
}

TextureFormat cookedTextureFormat(TextureUsage usage) {
    return usage == TextureUsage::Normal ? TextureFormat::Bc5 : TextureFormat::Bc7;
}

bool textureFormatSupported(TextureFormat format, const TextureFormatCapabilities& capabilities) {
    switch (format) {
    case TextureFormat::Bc7:
        return capabilities.bc7;
    case TextureFormat::Bc5:
        return capabilities.bc5;
    case TextureFormat::Rgba8:
    default:
        return true;
    }
}

bool isBlockCompressedKtx2Format(uint32_t vkFormat) {
    return vkFormat == ktx2FormatBc5Unorm || vkFormat == ktx2FormatBc7Unorm || vkFormat == ktx2FormatBc7Srgb;
}

AssetPipelineResult validateBlockCompressedTexture(const TextureAsset& texture) {
    if (!texture.blockCompressed()) {
        return {.error = "texture is not block-compressed"};
    }
    if (texture.width == 0 || texture.height == 0) {
        return {.error = "block-compressed texture has an empty base level"};
    }
    if (texture.mips.empty() || texture.mips.size() > fullMipLevelCount(texture.width, texture.height)) {
        return {.error = "block-compressed texture has an invalid mip count"};
    }
    uint64_t expectedOffset = 0;
    for (size_t level = 0; level < texture.mips.size(); ++level) {
        const TextureMipLevel& mip = texture.mips[level];
        const uint32_t width = std::max(texture.width >> level, 1u);
        const uint32_t height = std::max(texture.height >> level, 1u);
        if (mip.width != width || mip.height != height || mip.offset != expectedOffset ||
            mip.size != blockCompressedLevelBytes(width, height)) {
            return {.error = "block-compressed mip " + std::to_string(level) + " has an invalid extent or range"};
        }
        expectedOffset += mip.size;
    }
    if (expectedOffset != texture.compressed.size()) {
        return {.error = "block-compressed payload size does not match its mip table"};
    }
    return {.success = true};
}

std::vector<uint8_t> writeBlockCompressedKtx2(const TextureAsset& texture) {
    if (!validateBlockCompressedTexture(texture)) {
        return {};
    }
    const size_t levelCount = texture.mips.size();
    const std::vector<uint8_t> descriptor = dataFormatDescriptor(texture);
    const size_t descriptorOffset = ktx2HeaderBytes + ktx2LevelIndexEntryBytes * levelCount;
    size_t cursor = descriptorOffset + descriptor.size();

    // Level data is stored smallest level first, each aligned to the block size.
    std::vector<size_t> levelOffsets(levelCount);
    for (size_t level = levelCount; level-- > 0;) {
        cursor = alignUp(cursor, blockBytes);
        levelOffsets[level] = cursor;
        cursor += static_cast<size_t>(texture.mips[level].size);
    }

    std::vector<uint8_t> bytes(cursor, 0);
    std::copy(ktx2Identifier.begin(), ktx2Identifier.end(), bytes.begin());
    write32(bytes, 12, ktx2Format(texture));
    write32(bytes, 16, 1); // typeSize is 1 for block-compressed formats.
    write32(bytes, 20, texture.width);
    write32(bytes, 24, texture.height);
    write32(bytes, 28, 0); // pixelDepth: 2D
    write32(bytes, 32, 0); // layerCount: not an array
    write32(bytes, 36, 1); // faceCount
    write32(bytes, 40, static_cast<uint32_t>(levelCount));
    write32(bytes, 44, 0); // No supercompression.
    write32(bytes, 48, static_cast<uint32_t>(descriptorOffset));
    write32(bytes, 52, static_cast<uint32_t>(descriptor.size()));
    for (size_t level = 0; level < levelCount; ++level) {
        const size_t entry = ktx2HeaderBytes + ktx2LevelIndexEntryBytes * level;
        write64(bytes, entry, levelOffsets[level]);
        write64(bytes, entry + 8, texture.mips[level].size);
        write64(bytes, entry + 16, texture.mips[level].size);
        const auto source = texture.compressed.begin() + static_cast<std::ptrdiff_t>(texture.mips[level].offset);
        std::copy(source, source + static_cast<std::ptrdiff_t>(texture.mips[level].size),
                  bytes.begin() + static_cast<std::ptrdiff_t>(levelOffsets[level]));
    }
    std::copy(descriptor.begin(), descriptor.end(), bytes.begin() + static_cast<std::ptrdiff_t>(descriptorOffset));
    return bytes;
}

BlockCompressedDecodeResult readBlockCompressedKtx2(std::span<const uint8_t> bytes, std::string_view label) {
    const auto fail = [label](std::string_view reason) {
        return BlockCompressedDecodeResult{.error =
                                               "Cannot decode KTX2 " + std::string(label) + ": " + std::string(reason)};
    };
    if (bytes.size() < ktx2HeaderBytes || !std::equal(ktx2Identifier.begin(), ktx2Identifier.end(), bytes.begin())) {
        return fail("invalid or truncated header");
    }
    const uint32_t vkFormat = read32(bytes, 12);
    if (!isBlockCompressedKtx2Format(vkFormat)) {
        return fail("format is not BC5 or BC7");
    }
    TextureAsset texture{.width = read32(bytes, 20), .height = read32(bytes, 24)};
    const uint32_t depth = read32(bytes, 28);
    const uint32_t layers = read32(bytes, 32);
    const uint32_t faces = read32(bytes, 36);
    const uint32_t levels = read32(bytes, 40);
    if (texture.width == 0 || texture.height == 0 || depth != 0 || layers != 0 || faces != 1) {
        return fail("only single 2D images are supported");
    }
    if (read32(bytes, 44) != 0) {
        return fail("supercompressed block data is not supported");
    }
    if (levels == 0 || levels > fullMipLevelCount(texture.width, texture.height) ||
        bytes.size() < ktx2HeaderBytes + ktx2LevelIndexEntryBytes * size_t(levels)) {
        return fail("invalid mip level index");
    }
    texture.format = vkFormat == ktx2FormatBc5Unorm ? TextureFormat::Bc5 : TextureFormat::Bc7;
    texture.usage = vkFormat == ktx2FormatBc5Unorm  ? TextureUsage::Normal
                    : vkFormat == ktx2FormatBc7Srgb ? TextureUsage::Color
                                                    : TextureUsage::Data;
    texture.mips.reserve(levels);
    uint64_t payloadBytes = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t width = std::max(texture.width >> level, 1u);
        const uint32_t height = std::max(texture.height >> level, 1u);
        const uint64_t expected = blockCompressedLevelBytes(width, height);
        const size_t entry = ktx2HeaderBytes + ktx2LevelIndexEntryBytes * level;
        const uint64_t offset = read64(bytes, entry);
        const uint64_t length = read64(bytes, entry + 8);
        if (length != expected || offset > bytes.size() || length > bytes.size() - offset) {
            return fail("mip level " + std::to_string(level) + " is truncated or mis-sized");
        }
        texture.mips.push_back({.width = width, .height = height, .offset = payloadBytes, .size = length});
        payloadBytes += length;
    }
    texture.compressed.reserve(static_cast<size_t>(payloadBytes));
    for (uint32_t level = 0; level < levels; ++level) {
        const size_t entry = ktx2HeaderBytes + ktx2LevelIndexEntryBytes * level;
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(read64(bytes, entry));
        texture.compressed.insert(texture.compressed.end(), begin,
                                  begin + static_cast<std::ptrdiff_t>(texture.mips[level].size));
    }
    return {.texture = std::move(texture)};
}

DerivedAssetRequest cookedTextureRequest(const std::filesystem::path& source, TextureUsage usage) {
    const std::string format = cookedTextureFormat(usage) == TextureFormat::Bc5 ? "bc5" : "bc7";
    return {.source = source,
            .importSettings = "texture;usage=" + std::string(usageName(usage)) + ";format=" + format + ";mips=full",
            .toolVersion = std::string(cookedTextureToolVersion),
            .targetPlatform = std::string(cookedTexturePlatform)};
}

CookedTextureLookup loadCookedTexture(const std::filesystem::path& derivedRoot, const std::filesystem::path& source,
                                      TextureUsage usage) {
    const DerivedAssetCache cache(derivedRoot);
    std::string key;
    DerivedAssetRecord record;
    if (!cache.key(cookedTextureRequest(source, usage), key) || !cache.inspect(key, record)) {
        return {};
    }
    std::vector<uint8_t> bytes;
    readFileBytes(record.path, bytes);
    if (const AssetPipelineResult valid = validateKtx2(bytes); !valid) {
        return {.error = "Cooked texture " + record.path.string() + " is invalid: " + valid.error};
    }
    BlockCompressedDecodeResult decoded = readBlockCompressedKtx2(bytes, record.path.string());
    if (!decoded) {
        return {.error = decoded.error};
    }
    if (decoded.texture.format != cookedTextureFormat(usage)) {
        return {.error = "Cooked texture " + record.path.string() + " does not match its usage"};
    }
    decoded.texture.usage = usage;
    return {.texture = std::move(decoded.texture)};
}
