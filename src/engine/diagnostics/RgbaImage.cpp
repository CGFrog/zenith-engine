#include "RgbaImage.hpp"

#include <limits>
#include <stb_image.h>
#include <stb_image_write.h>
#include <system_error>

namespace {
constexpr uint32_t maximumImageDimension = static_cast<uint32_t>(std::numeric_limits<int>::max() / 4);
}

ImageFileResult writePng(const std::filesystem::path& path, const RgbaImage& image) {
    if (!image.valid()) {
        return {.error = "Cannot write " + path.string() + ": the image is empty or its pixel count is inconsistent"};
    }
    if (image.width > maximumImageDimension || image.height > maximumImageDimension) {
        return {.error = "Cannot write " + path.string() + ": the image is too large for PNG encoding"};
    }
    if (path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return {.error = "Cannot create " + path.parent_path().string() + ": " + error.message()};
        }
    }
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    const int rowBytes = width * static_cast<int>(RgbaImage::channelCount);
    if (stbi_write_png(path.string().c_str(), width, height, static_cast<int>(RgbaImage::channelCount),
                       image.pixels.data(), rowBytes) == 0) {
        return {.error = "Cannot write PNG " + path.string()};
    }
    return {};
}

ImageFileResult loadPng(const std::filesystem::path& path, RgbaImage& image) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        return {.error = "Cannot load image " + path.string() + ": " + (reason ? reason : "unknown error")};
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    const size_t byteCount = static_cast<size_t>(image.width) * image.height * RgbaImage::channelCount;
    image.pixels.assign(pixels, pixels + byteCount);
    stbi_image_free(pixels);
    return {};
}
