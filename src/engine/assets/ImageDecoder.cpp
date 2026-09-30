#define STB_IMAGE_IMPLEMENTATION
#include "ImageDecoder.hpp"
#include "ProductionAssetPipeline.hpp"

#include <stb_image.h>

#include <array>
#include <fstream>
#include <limits>

namespace {
ImageDecodeResult makeResult(stbi_uc* pixels, int width, int height, std::string_view label) {
    if (!pixels || width <= 0 || height <= 0) {
        return {.error = "Cannot decode image " + std::string(label) + ": " + stbi_failure_reason()};
    }

    TextureAsset texture;
    texture.width = static_cast<uint32_t>(width);
    texture.height = static_cast<uint32_t>(height);
    texture.rgba.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(pixels);
    return {.texture = std::move(texture)};
}

uint32_t read32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8u) | (uint32_t(bytes[2]) << 16u) | (uint32_t(bytes[3]) << 24u);
}

uint64_t read64(const uint8_t* bytes) {
    return uint64_t(read32(bytes)) | (uint64_t(read32(bytes + 4)) << 32u);
}

ImageDecodeResult decodeKtx2(std::span<const uint8_t> bytes, std::string_view label) {
    if (const AssetPipelineResult validation = validateKtx2(bytes); !validation) {
        return {.error = "Cannot decode KTX2 " + std::string(label) + ": " + validation.error};
    }
    const uint32_t vkFormat = read32(bytes.data() + 12);
    const uint32_t width = read32(bytes.data() + 20);
    const uint32_t height = read32(bytes.data() + 24);
    const uint32_t levels = read32(bytes.data() + 40);
    const uint32_t supercompression = read32(bytes.data() + 44);
    if (vkFormat == 0 || supercompression != 0) {
        return {.error = "Cannot decode Basis Universal KTX2 " + std::string(label) +
                         ": optional Basis transcoder is not installed"};
    }
    if (vkFormat != 37 && vkFormat != 43) {
        return {.error = "Cannot decode KTX2 " + std::string(label) +
                         ": only RGBA8 UNORM/SRGB has a CPU fallback on this build"};
    }
    if (levels == 0 || bytes.size() < 104) {
        return {.error = "Cannot decode KTX2: missing level index"};
    }
    const uint64_t offset = read64(bytes.data() + 80);
    const uint64_t length = read64(bytes.data() + 88);
    const uint64_t required = uint64_t(width) * height * 4;
    if (length < required || offset > bytes.size() || required > bytes.size() - offset) {
        return {.error = "Cannot decode KTX2 " + std::string(label) + ": truncated RGBA8 level"};
    }
    TextureAsset texture{width, height};
    texture.rgba.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset + required));
    return {.texture = std::move(texture)};
}
} // namespace

ImageDecodeResult decodeImageFile(const std::filesystem::path& path) {
    if (path.extension() == ".ktx2") {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return {.error = "Cannot open KTX2 image " + path.string()};
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return decodeKtx2(bytes, path.string());
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info(path.string().c_str(), &width, &height, &channels)) {
        return {.error = "Cannot inspect image " + path.string() + ": " + stbi_failure_reason()};
    }
    if (const AssetPipelineResult limits = validateAssetLimits({.decodedBytes = uint64_t(width) * uint64_t(height) * 4,
                                                                .textureWidth = static_cast<uint32_t>(width),
                                                                .textureHeight = static_cast<uint32_t>(height)});
        !limits) {
        return {.error = "Cannot decode image " + path.string() + ": " + limits.error};
    }
    stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    return makeResult(pixels, width, height, path.string());
}

ImageDecodeResult decodeImageMemory(const void* bytes, size_t size, std::string_view label) {
    const auto view = std::span(static_cast<const uint8_t*>(bytes), size);
    constexpr std::array<uint8_t, 12> ktx2Identifier{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                     0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    if (size >= ktx2Identifier.size() && std::equal(ktx2Identifier.begin(), ktx2Identifier.end(), view.begin())) {
        return decodeKtx2(view, label);
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    if (size > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !stbi_info_from_memory(static_cast<const stbi_uc*>(bytes), static_cast<int>(size), &width, &height,
                               &channels)) {
        return {.error = "Cannot inspect image " + std::string(label) + ": " + stbi_failure_reason()};
    }
    if (const AssetPipelineResult limits = validateAssetLimits({.sourceBytes = size,
                                                                .decodedBytes = uint64_t(width) * uint64_t(height) * 4,
                                                                .textureWidth = static_cast<uint32_t>(width),
                                                                .textureHeight = static_cast<uint32_t>(height)});
        !limits) {
        return {.error = "Cannot decode image " + std::string(label) + ": " + limits.error};
    }
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes), static_cast<int>(size), &width, &height,
                                            &channels, STBI_rgb_alpha);
    return makeResult(pixels, width, height, label);
}
