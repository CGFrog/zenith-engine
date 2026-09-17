#define STB_IMAGE_IMPLEMENTATION
#include "ImageDecoder.hpp"

#include <stb_image.h>

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
} // namespace

ImageDecodeResult decodeImageFile(const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    return makeResult(pixels, width, height, path.string());
}

ImageDecodeResult decodeImageMemory(const void* bytes, size_t size, std::string_view label) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes), static_cast<int>(size), &width, &height,
                                            &channels, STBI_rgb_alpha);
    return makeResult(pixels, width, height, label);
}
