#include "editor/theme/EditorPalette.hpp"
#include "editor/theme/PixelIconBitmaps.hpp"
#include "editor/theme/PixelWallpaper.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr float textContrast = 4.5f;
constexpr float graphicContrast = 3.0f;

ThemeColor pixelAt(const PixelImage& image, size_t index) {
    const uint8_t* rgba = &image.rgba[index * 4u];
    return {static_cast<float>(rgba[0]) / 255.0f, static_cast<float>(rgba[1]) / 255.0f,
            static_cast<float>(rgba[2]) / 255.0f, 1.0f};
}

void testContrastHelpers() {
    const ThemeColor black = ThemeColor::hex(0x000000);
    const ThemeColor white = ThemeColor::hex(0xFFFFFF);
    expect(std::abs(contrastRatio(black, white) - 21.0f) < 0.01f, "black on white is 21:1");
    expect(std::abs(contrastRatio(white, white) - 1.0f) < 0.001f, "a colour on itself is 1:1");
    const ThemeColor half = composite(white.withAlpha(0.5f), black);
    expect(std::abs(half.r - 0.5f) < 0.001f && half.a == 1.0f, "compositing mixes by alpha into an opaque colour");
}

// Text the panes, fields, headers, buttons, and rows carry stays readable.
void testTextContrast() {
    using namespace palette;
    const ThemeColor rows[] = {pane,       paneAlternate,         field, fieldHovered, rowSelected,
                               rowHovered, composite(popup, pane)};
    for (const ThemeColor surface : rows) {
        for (const ThemeColor text : {ink, success, info, warning, error}) {
            expect(contrastRatio(text, surface) >= textContrast, "text and severity colours reach 4.5:1 on rows");
        }
    }
    for (const ThemeColor surface :
         {sectionTop, sectionBottom, buttonTop, buttonBottom, buttonHoverTop, buttonHoverBottom}) {
        expect(contrastRatio(ink, surface) >= textContrast, "ink reaches 4.5:1 on headers and buttons");
    }
    for (const ThemeColor surface : {pane, field, paneAlternate}) {
        expect(contrastRatio(inkMuted, surface) >= textContrast, "muted text reaches 4.5:1 on panes and fields");
        expect(contrastRatio(inkDisabled, surface) >= graphicContrast, "disabled text stays distinguishable");
        expect(contrastRatio(pixelGlow, surface) >= graphicContrast, "the glow ring stands out from panes");
    }
    for (const ThemeColor surface : {sectionTop, sectionBottom}) {
        expect(contrastRatio(sectionInk, surface) >= textContrast, "section titles read on their bar");
    }
    for (const ThemeColor surface : {buttonActiveTop, buttonActiveBottom}) {
        expect(contrastRatio(activeInk, surface) >= textContrast, "lit buttons keep dark text readable");
    }
    for (const ThemeColor axis : {axisX, axisY, axisZ, activeObjectLabel}) {
        expect(contrastRatio(axis, composite(smokedGlass, pane)) >= graphicContrast,
               "axis and label colours stand out from smoked glass");
    }
}

// Caption text is outlined, so one of ink and outline must contrast with
// every frosted wallpaper cell it may sit on.
void testCaptionContrastOverFrostedGlass() {
    const WallpaperImages images = generateNightWallpaper(160, 90);
    float worst = 21.0f;
    for (size_t index = 0; index < static_cast<size_t>(images.frosted.width) * images.frosted.height; ++index) {
        const ThemeColor cell = pixelAt(images.frosted, index);
        worst = std::min(
            worst, std::max(contrastRatio(palette::captionInk, cell), contrastRatio(palette::captionOutline, cell)));
    }
    expect(worst >= textContrast, "outlined captions read over every frosted wallpaper cell");
}

void testWallpaperIsDeterministicAndPaletted() {
    const WallpaperImages first = generateNightWallpaper(96, 54);
    const WallpaperImages second = generateNightWallpaper(96, 54);
    expect(first.sharp.rgba == second.sharp.rgba && first.frosted.rgba == second.frosted.rgba,
           "the same size always produces the same wallpaper");
    expect(first.sharp.width == 96 && first.sharp.height == 54 && first.sharp.rgba.size() == 96u * 54u * 4u,
           "the wallpaper covers the requested grid");
    bool paletted = true;
    for (const PixelImage* image : {&first.sharp, &first.frosted}) {
        for (size_t index = 0; index < static_cast<size_t>(image->width) * image->height; ++index) {
            const ThemeColor cell = pixelAt(*image, index);
            paletted = paletted && std::ranges::any_of(palette::wallpaper, [&](const ThemeColor& entry) {
                           return std::abs(entry.r - cell.r) < 0.003f && std::abs(entry.g - cell.g) < 0.003f &&
                                  std::abs(entry.b - cell.b) < 0.003f && image->rgba[index * 4u + 3u] == 255;
                       });
        }
    }
    expect(paletted, "every wallpaper pixel is an opaque palette colour");
}

float neighbourVariation(const SceneImage& image) {
    float total = 0.0f;
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 1; x < image.width; ++x) {
            const SceneImage::Pixel& left = image.pixels[y * image.width + x - 1];
            const SceneImage::Pixel& right = image.pixels[y * image.width + x];
            total += std::abs(left.r - right.r) + std::abs(left.g - right.g) + std::abs(left.b - right.b);
        }
    }
    return total;
}

void testFrostSoftensTheScene() {
    const SceneImage scene = paintNightScene(120, 68);
    const SceneImage frosted = frost(scene, 3, palette::glassTint, 0.0f);
    expect(neighbourVariation(frosted) < neighbourVariation(scene) * 0.5f, "frosting blurs away fine detail");
    const SceneImage tinted = frost(scene, 0, palette::glassTint, 1.0f);
    expect(std::abs(tinted.pixels.front().b - palette::glassTint.b) < 0.001f, "a full tint replaces the colour");
}

void testDegenerateWallpaperSizes() {
    for (const auto& [width, height] : {std::pair<uint32_t, uint32_t>{1, 1}, {3, 5}, {7, 2}}) {
        const WallpaperImages images = generateNightWallpaper(width, height);
        expect(images.sharp.rgba.size() == static_cast<size_t>(width) * height * 4u &&
                   images.frosted.rgba.size() == images.sharp.rgba.size(),
               "tiny and odd wallpaper sizes are produced whole");
    }
    expect(generateNightWallpaper(0, 4).sharp.rgba.empty(), "an empty grid produces no pixels");
}

void testIconBitmaps() {
    for (size_t index = 0; index < editorIconCount; ++index) {
        const IconBitmap& bitmap = iconBitmap(static_cast<EditorIcon>(index));
        bool valid = true;
        bool outlined = false;
        for (const std::string_view row : bitmap.rows) {
            valid = valid && row.size() == iconBitmapSize;
            for (const char pixel : row) {
                valid = valid && (pixel == '.' || iconPixelColor(bitmap, pixel).has_value());
                outlined = outlined || pixel == 'o';
            }
        }
        expect(valid, "icon " + std::to_string(index) + " is a square of known pixels");
        expect(outlined, "icon " + std::to_string(index) + " has an outline");
    }
    expect(!iconPixelColor(iconBitmap(EditorIcon::Close), '.').has_value(), "'.' is transparent");
}
} // namespace

int main() {
    testContrastHelpers();
    testTextContrast();
    testCaptionContrastOverFrostedGlass();
    testWallpaperIsDeterministicAndPaletted();
    testFrostSoftensTheScene();
    testDegenerateWallpaperSizes();
    testIconBitmaps();
    if (failures != 0) {
        std::cerr << failures << " editor palette test(s) failed\n";
        return 1;
    }
    std::cout << "Editor palette tests passed\n";
    return 0;
}
