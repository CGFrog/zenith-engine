#include "engine/assets/TextureCooker.hpp"

#include "engine/assets/CompressedTexture.hpp"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <thread>

namespace {
constexpr uint32_t blockExtent = 4;
constexpr size_t blockBytes = 16;

struct RgbaImage {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> pixels;
};

void initializeEncoders() {
    // Both encoders build global lookup tables that must exist before any
    // block is encoded, and must not be rebuilt while encoding.
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

float srgbToLinear(uint8_t value) {
    const float normalized = value / 255.0f;
    return normalized <= 0.04045f ? normalized / 12.92f : std::pow((normalized + 0.055f) / 1.055f, 2.4f);
}

uint8_t linearToSrgb(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    const float encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(encoded * 255.0f));
}

uint8_t unitToByte(float value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

// Averages the 2x2 footprint of each destination texel (edge texels repeat
// for odd extents) in the space that is linear for the usage.
RgbaImage downsample(const RgbaImage& source, TextureUsage usage) {
    static const std::array<float, 256> srgbTable = [] {
        std::array<float, 256> table{};
        for (uint32_t value = 0; value < table.size(); ++value) {
            table[value] = srgbToLinear(static_cast<uint8_t>(value));
        }
        return table;
    }();
    RgbaImage result{.width = std::max(source.width / 2, 1u), .height = std::max(source.height / 2, 1u)};
    result.pixels.resize(size_t(result.width) * result.height * 4);
    for (uint32_t y = 0; y < result.height; ++y) {
        for (uint32_t x = 0; x < result.width; ++x) {
            std::array<float, 4> sum{};
            for (uint32_t sample = 0; sample < 4; ++sample) {
                const uint32_t sourceX = std::min(x * 2 + (sample & 1u), source.width - 1);
                const uint32_t sourceY = std::min(y * 2 + (sample >> 1u), source.height - 1);
                const uint8_t* texel = &source.pixels[(size_t(sourceY) * source.width + sourceX) * 4];
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    const bool srgb = usage == TextureUsage::Color && channel < 3;
                    sum[channel] += srgb ? srgbTable[texel[channel]] : texel[channel] / 255.0f;
                }
            }
            uint8_t* output = &result.pixels[(size_t(y) * result.width + x) * 4];
            if (usage == TextureUsage::Normal) {
                // Average unit vectors, then renormalize so lower levels stay unit length.
                float normalX = sum[0] * 0.5f - 1.0f;
                float normalY = sum[1] * 0.5f - 1.0f;
                float normalZ = sum[2] * 0.5f - 1.0f;
                const float length = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
                if (length > 1.0e-6f) {
                    normalX /= length;
                    normalY /= length;
                    normalZ /= length;
                } else {
                    normalX = 0.0f;
                    normalY = 0.0f;
                    normalZ = 1.0f;
                }
                output[0] = unitToByte(normalX * 0.5f + 0.5f);
                output[1] = unitToByte(normalY * 0.5f + 0.5f);
                output[2] = unitToByte(normalZ * 0.5f + 0.5f);
                output[3] = 255;
                continue;
            }
            for (uint32_t channel = 0; channel < 4; ++channel) {
                const float average = sum[channel] * 0.25f;
                const bool srgb = usage == TextureUsage::Color && channel < 3;
                output[channel] = srgb ? linearToSrgb(average) : unitToByte(average);
            }
        }
    }
    return result;
}

// Copies one 4x4 block, repeating edge texels for partial blocks.
std::array<uint8_t, 64> gatherBlock(const RgbaImage& image, uint32_t blockX, uint32_t blockY) {
    std::array<uint8_t, 64> block{};
    for (uint32_t row = 0; row < blockExtent; ++row) {
        const uint32_t y = std::min(blockY * blockExtent + row, image.height - 1);
        for (uint32_t column = 0; column < blockExtent; ++column) {
            const uint32_t x = std::min(blockX * blockExtent + column, image.width - 1);
            std::copy_n(&image.pixels[(size_t(y) * image.width + x) * 4], 4, &block[(row * blockExtent + column) * 4]);
        }
    }
    return block;
}

void encodeLevel(const RgbaImage& image, TextureUsage usage, uint8_t* destination) {
    const uint32_t blocksWide = (image.width + blockExtent - 1) / blockExtent;
    const uint32_t blocksHigh = (image.height + blockExtent - 1) / blockExtent;
    bc7enc_compress_block_params bc7Params;
    bc7enc_compress_block_params_init(&bc7Params);
    if (usage == TextureUsage::Data) {
        bc7enc_compress_block_params_init_linear_weights(&bc7Params);
    }
    const auto encodeRows = [&](uint32_t firstRow, uint32_t endRow) {
        for (uint32_t blockY = firstRow; blockY < endRow; ++blockY) {
            for (uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
                const std::array<uint8_t, 64> pixels = gatherBlock(image, blockX, blockY);
                uint8_t* block = destination + (size_t(blockY) * blocksWide + blockX) * blockBytes;
                if (usage == TextureUsage::Normal) {
                    rgbcx::encode_bc5_hq(block, pixels.data(), 0, 1, 4);
                } else {
                    bc7enc_compress_block(block, pixels.data(), &bc7Params);
                }
            }
        }
    };
    // Rows are independent, so split them across threads; output stays deterministic.
    const uint32_t threadCount = std::clamp(std::thread::hardware_concurrency(), 1u, std::max(blocksHigh / 8, 1u));
    std::vector<std::jthread> workers;
    workers.reserve(threadCount);
    for (uint32_t thread = 0; thread < threadCount; ++thread) {
        workers.emplace_back(encodeRows, blocksHigh * thread / threadCount, blocksHigh * (thread + 1) / threadCount);
    }
}
} // namespace

TextureCookResult cookBlockCompressedTexture(const TextureAsset& source, TextureUsage usage) {
    if (source.blockCompressed()) {
        return {.error = "texture is already block-compressed"};
    }
    if (source.width == 0 || source.height == 0 || source.rgba.size() != size_t(source.width) * source.height * 4) {
        return {.error = "texture has no RGBA8 base level"};
    }
    initializeEncoders();
    TextureCookResult result;
    TextureAsset& cooked = result.texture;
    cooked.width = source.width;
    cooked.height = source.height;
    cooked.format = cookedTextureFormat(usage);
    cooked.usage = usage;
    const uint32_t levelCount = fullMipLevelCount(source.width, source.height);
    uint64_t totalBytes = 0;
    for (uint32_t level = 0; level < levelCount; ++level) {
        const uint32_t width = std::max(source.width >> level, 1u);
        const uint32_t height = std::max(source.height >> level, 1u);
        const uint64_t bytes = blockCompressedLevelBytes(width, height);
        cooked.mips.push_back({.width = width, .height = height, .offset = totalBytes, .size = bytes});
        totalBytes += bytes;
    }
    cooked.compressed.resize(static_cast<size_t>(totalBytes));

    RgbaImage level{.width = source.width, .height = source.height, .pixels = source.rgba};
    for (uint32_t index = 0; index < levelCount; ++index) {
        if (index > 0) {
            level = downsample(level, usage);
        }
        encodeLevel(level, usage, cooked.compressed.data() + cooked.mips[index].offset);
    }
    return result;
}

std::vector<uint8_t> decodeBlockCompressedLevel(const TextureAsset& texture, uint32_t level) {
    if (!validateBlockCompressedTexture(texture) || level >= texture.mips.size()) {
        return {};
    }
    const TextureMipLevel& mip = texture.mips[level];
    const uint32_t blocksWide = (mip.width + blockExtent - 1) / blockExtent;
    const uint32_t blocksHigh = (mip.height + blockExtent - 1) / blockExtent;
    std::vector<uint8_t> rgba(size_t(mip.width) * mip.height * 4);
    for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
            const uint8_t* block =
                texture.compressed.data() + mip.offset + (size_t(blockY) * blocksWide + blockX) * blockBytes;
            std::array<uint8_t, 64> pixels{};
            if (texture.format == TextureFormat::Bc5) {
                for (size_t pixel = 0; pixel < 16; ++pixel) {
                    pixels[pixel * 4 + 3] = 255;
                }
                rgbcx::unpack_bc5(block, pixels.data(), 0, 1, 4);
            } else {
                bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(pixels.data()));
            }
            for (uint32_t row = 0; row < blockExtent; ++row) {
                const uint32_t y = blockY * blockExtent + row;
                for (uint32_t column = 0; column < blockExtent; ++column) {
                    const uint32_t x = blockX * blockExtent + column;
                    if (x < mip.width && y < mip.height) {
                        std::copy_n(&pixels[(row * blockExtent + column) * 4], 4,
                                    &rgba[(size_t(y) * mip.width + x) * 4]);
                    }
                }
            }
        }
    }
    return rgba;
}
