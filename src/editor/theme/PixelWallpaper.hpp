#pragma once

#include "editor/theme/EditorPalette.hpp"

#include <cstdint>
#include <span>
#include <vector>

// The procedural night wallpaper behind the editor's glass. Everything here
// is pure CPU and deterministic: the same size always produces the same
// pixels, so it can be tested without a GPU.

// A continuous-tone image in sRGB-encoded floats, one entry per wallpaper cell.
struct SceneImage {
    struct Pixel {
        float r{0.0f};
        float g{0.0f};
        float b{0.0f};
    };

    uint32_t width{0};
    uint32_t height{0};
    std::vector<Pixel> pixels;
};

// Tightly packed RGBA8 rows, top row first.
struct PixelImage {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba;
};

// The sharp wallpaper and its frosted copy (blurred and tinted toward the
// glass colour), both quantized to the wallpaper palette.
struct WallpaperImages {
    PixelImage sharp;
    PixelImage frosted;
};

// Paints the moonlit night scene: sky, stars, aurora, moon, clouds, light
// swooshes, hills with fireflies, a lake, leaves, and bubbles. One pixel is
// one wallpaper cell.
SceneImage paintNightScene(uint32_t width, uint32_t height);
// Box-blurs the image twice with the given radius in cells (clamped at the
// edges), then mixes it toward tint by amount in [0, 1].
SceneImage frost(const SceneImage& image, uint32_t radius, ThemeColor tint, float amount);
// Maps every pixel to its nearest palette colour after a 4x4 ordered (Bayer)
// dither offset, so gradients become retro bands.
PixelImage quantize(const SceneImage& image, std::span<const ThemeColor> palette);
// Both wallpaper images for a grid of width x height cells; the frosted copy
// is blurred by frostRadius cells and mixed toward the glass tint by
// tintAmount (the defaults are the theme's own glass).
WallpaperImages generateNightWallpaper(uint32_t width, uint32_t height, uint32_t frostRadius = 3,
                                       float tintAmount = palette::glassTintAmount);
