#pragma once

// An 8-bit RGBA image in row-major order, top row first, four bytes per pixel.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// An 8-bit RGBA image in row-major order, top row first, four bytes per pixel.
// Color values are stored as encoded (sRGB for swapchain captures); nothing in
// this type converts color spaces.
struct RgbaImage {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> pixels;

    static constexpr uint32_t channelCount = 4;

    bool valid() const {
        return width != 0 && height != 0 && pixels.size() == static_cast<size_t>(width) * height * channelCount;
    }
};

struct ImageFileResult {
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// Writes the image as an 8-bit RGBA PNG, creating missing parent directories.
ImageFileResult writePng(const std::filesystem::path& path, const RgbaImage& image);

// Loads any PNG (or other stb_image format) as 8-bit RGBA.
ImageFileResult loadPng(const std::filesystem::path& path, RgbaImage& image);
