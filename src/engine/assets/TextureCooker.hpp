#pragma once

#include "Assets.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct TextureCookResult {
    TextureAsset texture;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Offline texture cooking. Builds a full mip chain from an RGBA8 base level
// (sRGB-correct for color, renormalized for normals, linear for data) and
// block-compresses every level in the usage's cooked format: BC7 for color
// and data, BC5 (X and Y only) for tangent-space normals. Deterministic for a
// given input.
TextureCookResult cookBlockCompressedTexture(const TextureAsset& source, TextureUsage usage);

// Decodes one block-compressed level to tightly packed RGBA8. BC5 levels decode
// to (X, Y, 0, 255). Returns an empty vector for invalid input.
std::vector<uint8_t> decodeBlockCompressedLevel(const TextureAsset& texture, uint32_t level);
