#include "editor/theme/PixelWallpaper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace {
using Pixel = SceneImage::Pixel;

struct GradientStop {
    float position;
    uint32_t color;
};

struct Ellipse {
    float centerX;
    float centerY;
    float radiusX;
    float radiusY;
};

struct Leaf {
    float centerX;
    float centerY;
    float length;
    float angle;
};

struct Bubble {
    float centerX;
    float centerY;
    float radiusCells;
};

// Scene positions are fractions of the wallpaper; x is scaled by the aspect
// ratio wherever a shape must stay round.
constexpr float moonX = 0.8f;
constexpr float moonY = 0.14f;
constexpr std::array<Ellipse, 2> nightClouds{{{0.15f, 0.5f, 0.16f, 0.025f}, {0.55f, 0.53f, 0.14f, 0.02f}}};
constexpr std::array<Leaf, 4> leaves{{{0.02f, 0.3f, 0.07f, -0.6f},
                                      {0.04f, 0.45f, 0.045f, 0.7f},
                                      {0.98f, 0.6f, 0.08f, 1.4f},
                                      {0.95f, 0.8f, 0.045f, 2.4f}}};
constexpr std::array<Bubble, 6> bubbles{{{0.29f, 0.7f, 5.0f},
                                         {0.35f, 0.82f, 2.0f},
                                         {0.9f, 0.27f, 6.0f},
                                         {0.95f, 0.42f, 3.0f},
                                         {0.62f, 0.88f, 4.0f},
                                         {0.05f, 0.16f, 3.0f}}};
constexpr std::array<std::array<float, 2>, 6> fireflies{
    {{0.2f, 0.74f}, {0.27f, 0.79f}, {0.74f, 0.72f}, {0.82f, 0.77f}, {0.68f, 0.8f}, {0.12f, 0.83f}}};
constexpr std::array<int, 16> bayer{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// Strength of the ordered dither offset, in sRGB units.
constexpr float ditherStrength = 0.1f;

Pixel pixel(uint32_t hex) {
    const ThemeColor color = ThemeColor::hex(hex);
    return {color.r, color.g, color.b};
}

Pixel mix(Pixel from, Pixel to, float amount) {
    const float t = std::clamp(amount, 0.0f, 1.0f);
    return {from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t};
}

Pixel gradient(std::initializer_list<GradientStop> stops, float position) {
    const GradientStop* previous = stops.begin();
    for (const GradientStop* stop = stops.begin() + 1; stop != stops.end(); ++stop) {
        if (position <= stop->position) {
            const float span = stop->position - previous->position;
            const float t = span > 0.0f ? (position - previous->position) / span : 1.0f;
            return mix(pixel(previous->color), pixel(stop->color), t);
        }
        previous = stop;
    }
    return pixel(previous->color);
}

// Stable per-cell noise in [0, 1).
float hash(float x, float y) {
    const float value = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return value - std::floor(value);
}

float square(float value) {
    return value * value;
}

Pixel paintSky(float u, float v, float x, float y) {
    Pixel color =
        gradient({{0.0f, 0x01040E}, {0.3f, 0x041030}, {0.55f, 0x0A2A55}, {0.66f, 0x123C6A}}, std::min(v, 0.66f));
    // Aurora: a wobbling ribbon, mint above its centre line and violet below.
    const float auroraLine = 0.2f + 0.06f * std::sin(u * 9.0f) + 0.03f * std::sin(u * 23.0f);
    const float auroraDistance = std::abs(v - auroraLine);
    if (auroraDistance < 0.09f && u > 0.05f && u < 0.75f) {
        const float intensity = (1.0f - auroraDistance / 0.09f) * (0.55f + 0.45f * std::sin(u * 14.0f));
        color = mix(color, pixel(v < auroraLine ? 0x5EF0C0 : 0x6A3AA0), std::max(0.0f, intensity) * 0.55f);
    }
    if (v < 0.6f && hash(x, y) > 0.985f) {
        color = mix(color, pixel(0xFFFFFF), 0.6f + 0.4f * hash(y, x));
    }
    if (v < 0.6f && hash(x + 3.0f, y + 7.0f) > 0.997f) {
        color = pixel(0xFFF2B0);
    }
    return color;
}

Pixel paintMoon(Pixel color, float u, float v, float aspect, float x, float y) {
    const float distance = std::hypot((u - moonX) * aspect, v - moonY);
    color = mix(color, pixel(0xE6F0FF), square(std::max(0.0f, 1.0f - distance / 0.22f)) * 0.45f);
    if (distance < 0.055f) {
        color = pixel(0xFFF8E0);
        if (distance < 0.045f && hash(x * 3.0f, y * 5.0f) > 0.85f) {
            color = mix(color, pixel(0xC8D4F0), 0.6f);
        }
    }
    return color;
}

Pixel paintLandscape(Pixel color, float u, float v, float aspect, float x, uint32_t row) {
    for (const Ellipse& cloud : nightClouds) {
        const float extent = square((u - cloud.centerX) / cloud.radiusX) + square((v - cloud.centerY) / cloud.radiusY);
        if (extent < 1.0f) {
            color = mix(color, pixel(0x2A4A7A), (1.0f - extent) * 0.5f);
        }
    }
    // Two light swooshes: arcs of one large circle below the wallpaper.
    const float swoosh = std::hypot((u - 0.5f) * aspect, v - 1.75f);
    if (std::abs(swoosh - 1.32f) < 0.012f) {
        color = mix(color, pixel(0xB6FFE6), 0.35f);
    }
    if (std::abs(swoosh - 1.25f) < 0.008f) {
        color = mix(color, pixel(0x5EF0C0), 0.3f);
    }
    const float leftHill = square((u - 0.12f) / 0.55f) + square((v - 1.08f) / 0.42f);
    const float rightHill = square((u - 0.9f) / 0.6f) + square((v - 1.12f) / 0.47f);
    if (leftHill < 1.0f || rightHill < 1.0f) {
        const float depth = leftHill < 1.0f ? (v - 0.66f) / 0.42f : (v - 0.65f) / 0.47f;
        color = gradient({{0.0f, 0x1E8A80}, {0.15f, 0x0A4A40}, {0.5f, 0x04302A}, {1.0f, 0x021A16}}, depth);
        if (hash(x + 11.0f, static_cast<float>(row) + 5.0f) > 0.993f) {
            color = pixel(0xD0FF60);
        }
    }
    const float lake = square((u - 0.5f) / 0.3f) + square((v - 1.03f) / 0.2f);
    if (lake < 1.0f) {
        color = gradient({{0.83f, 0x1A4A7A}, {0.92f, 0x0A2350}, {1.0f, 0x030A1E}}, v);
        if (row % 4u == 0u) {
            color = mix(color, pixel(0x5EF0C0), 0.25f);
        }
        // The moon's broken reflection widens toward the viewer.
        if (std::abs(u - moonX) < 0.03f * (1.0f + (v - 0.83f) * 3.0f) && row % 3u != 1u) {
            color = mix(color, pixel(0xFFF2B0), 0.75f);
        }
    }
    for (const Leaf& leaf : leaves) {
        const float localX = (u - leaf.centerX) * aspect;
        const float localY = v - leaf.centerY;
        const float along = localX * std::cos(leaf.angle) + localY * std::sin(leaf.angle);
        const float across = -localX * std::sin(leaf.angle) + localY * std::cos(leaf.angle);
        const float width = leaf.length * 0.45f;
        if (square(along / leaf.length) + square(across / width) < 1.0f) {
            color = gradient({{0.0f, 0x5EF0C0}, {0.5f, 0x13606A}, {1.0f, 0x04302A}}, (across / width + 1.0f) * 0.5f);
            if (std::abs(across) < 0.006f) {
                color = mix(color, pixel(0xFFFFFF), 0.35f);
            }
        }
    }
    return color;
}

void setCell(SceneImage& image, int x, int y, Pixel color, float amount) {
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height)) {
        return;
    }
    Pixel& cell = image.pixels[static_cast<size_t>(y) * image.width + static_cast<size_t>(x)];
    cell = mix(cell, color, amount);
}

// Bubbles and firefly sparkles are drawn in whole cells so they stay sharp.
void paintSparkles(SceneImage& image) {
    for (const Bubble& bubble : bubbles) {
        const float centerX = std::round(bubble.centerX * static_cast<float>(image.width));
        const float centerY = std::round(bubble.centerY * static_cast<float>(image.height));
        const int reach = static_cast<int>(bubble.radiusCells) + 1;
        for (int y = -reach; y <= reach; ++y) {
            for (int x = -reach; x <= reach; ++x) {
                const float distance = std::hypot(static_cast<float>(x), static_cast<float>(y));
                if (std::abs(distance - bubble.radiusCells) < 0.5f) {
                    setCell(image, static_cast<int>(centerX) + x, static_cast<int>(centerY) + y, pixel(0xB6FFE6), 1.0f);
                }
            }
        }
        const int highlight = std::max(1, static_cast<int>(std::round(bubble.radiusCells / 3.0f)));
        const int offset = static_cast<int>(std::round(bubble.radiusCells / 2.0f));
        for (int y = 0; y < highlight; ++y) {
            for (int x = 0; x < highlight; ++x) {
                setCell(image, static_cast<int>(centerX) - offset + x, static_cast<int>(centerY) - offset + y,
                        pixel(0xFFFFFF), 1.0f);
            }
        }
    }
    for (const auto& [fireflyX, fireflyY] : fireflies) {
        const auto x = static_cast<int>(std::round(fireflyX * static_cast<float>(image.width)));
        const auto y = static_cast<int>(std::round(fireflyY * static_cast<float>(image.height)));
        setCell(image, x, y, pixel(0xD0FF60), 1.0f);
        for (const auto& [dx, dy] : std::array<std::array<int, 2>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
            setCell(image, x + dx, y + dy, pixel(0xD0FF60), 0.35f);
        }
    }
}

void blurRows(const SceneImage& source, SceneImage& target, int radius) {
    const int width = static_cast<int>(source.width);
    for (uint32_t y = 0; y < source.height; ++y) {
        const Pixel* row = &source.pixels[static_cast<size_t>(y) * source.width];
        for (int x = 0; x < width; ++x) {
            Pixel sum;
            for (int offset = -radius; offset <= radius; ++offset) {
                const Pixel& sample = row[std::clamp(x + offset, 0, width - 1)];
                sum.r += sample.r;
                sum.g += sample.g;
                sum.b += sample.b;
            }
            const auto count = static_cast<float>(radius * 2 + 1);
            target.pixels[static_cast<size_t>(y) * source.width + static_cast<size_t>(x)] = {
                sum.r / count, sum.g / count, sum.b / count};
        }
    }
}

void blurColumns(const SceneImage& source, SceneImage& target, int radius) {
    const int height = static_cast<int>(source.height);
    for (int y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < source.width; ++x) {
            Pixel sum;
            for (int offset = -radius; offset <= radius; ++offset) {
                const auto sampleRow = static_cast<size_t>(std::clamp(y + offset, 0, height - 1));
                const Pixel& sample = source.pixels[sampleRow * source.width + x];
                sum.r += sample.r;
                sum.g += sample.g;
                sum.b += sample.b;
            }
            const auto count = static_cast<float>(radius * 2 + 1);
            target.pixels[static_cast<size_t>(y) * source.width + x] = {sum.r / count, sum.g / count, sum.b / count};
        }
    }
}
} // namespace

SceneImage paintNightScene(uint32_t width, uint32_t height) {
    SceneImage image{.width = width, .height = height};
    image.pixels.resize(static_cast<size_t>(width) * height);
    if (width == 0 || height == 0) {
        return image;
    }
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width);
            const float v = static_cast<float>(y) / static_cast<float>(height);
            const auto cellX = static_cast<float>(x);
            const auto cellY = static_cast<float>(y);
            Pixel color = paintSky(u, v, cellX, cellY);
            color = paintMoon(color, u, v, aspect, cellX, cellY);
            color = paintLandscape(color, u, v, aspect, cellX, y);
            image.pixels[static_cast<size_t>(y) * width + x] = color;
        }
    }
    paintSparkles(image);
    return image;
}

SceneImage frost(const SceneImage& image, uint32_t radius, ThemeColor tint, float amount) {
    if (image.pixels.empty()) {
        return image;
    }
    SceneImage blurred = image;
    SceneImage scratch = image;
    const auto blurRadius = static_cast<int>(radius);
    for (int pass = 0; pass < 2; ++pass) {
        blurRows(blurred, scratch, blurRadius);
        blurColumns(scratch, blurred, blurRadius);
    }
    const Pixel target{tint.r, tint.g, tint.b};
    for (Pixel& cell : blurred.pixels) {
        cell = mix(cell, target, amount);
    }
    return blurred;
}

PixelImage quantize(const SceneImage& image, std::span<const ThemeColor> palette) {
    PixelImage result{.width = image.width, .height = image.height};
    result.rgba.resize(static_cast<size_t>(image.width) * image.height * 4u);
    if (palette.empty()) {
        return result;
    }
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const Pixel& cell = image.pixels[static_cast<size_t>(y) * image.width + x];
            const float offset = (static_cast<float>(bayer[(y % 4u) * 4u + x % 4u]) / 16.0f - 0.5f) * ditherStrength;
            const ThemeColor* nearest = palette.data();
            float nearestDistance = 1e9f;
            for (const ThemeColor& candidate : palette) {
                // Perceptual weights favour green, as the eye does.
                const float distance = 0.3f * square(cell.r + offset - candidate.r) +
                                       0.59f * square(cell.g + offset - candidate.g) +
                                       0.11f * square(cell.b + offset - candidate.b);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearest = &candidate;
                }
            }
            uint8_t* out = &result.rgba[(static_cast<size_t>(y) * image.width + x) * 4u];
            out[0] = static_cast<uint8_t>(std::lround(nearest->r * 255.0f));
            out[1] = static_cast<uint8_t>(std::lround(nearest->g * 255.0f));
            out[2] = static_cast<uint8_t>(std::lround(nearest->b * 255.0f));
            out[3] = 255;
        }
    }
    return result;
}

WallpaperImages generateNightWallpaper(uint32_t width, uint32_t height) {
    const SceneImage scene = paintNightScene(width, height);
    return {.sharp = quantize(scene, palette::wallpaper),
            .frosted = quantize(frost(scene, 3, palette::glassTint, palette::glassTintAmount), palette::wallpaper)};
}
