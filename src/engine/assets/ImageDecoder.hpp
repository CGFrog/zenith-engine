#pragma once

// Decodes image files to CPU pixels (stb_image); Radiance HDR stays linear.

#include "Assets.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

struct ImageDecodeResult {
    TextureAsset texture;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

ImageDecodeResult decodeImageFile(const std::filesystem::path& path);
ImageDecodeResult decodeImageMemory(const void* bytes, size_t size, std::string_view label);
