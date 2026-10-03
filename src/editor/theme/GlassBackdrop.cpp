#include "editor/theme/GlassBackdrop.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <chrono>
#include <cmath>
#include <utility>

namespace {
// Frames a new display size must hold before the wallpaper regenerates.
constexpr uint32_t stableFramesBeforeRegenerating = 12;

UiImage toUiImage(PixelImage image) {
    return {
        .width = image.width, .height = image.height, .rgba = std::move(image.rgba), .filter = UiImageFilter::Nearest};
}
} // namespace

GlassBackdrop::~GlassBackdrop() {
    // The future's destructor would wait anyway; waiting here keeps that explicit.
    if (m_generation.valid()) {
        m_generation.wait();
    }
}

void GlassBackdrop::update(RenderRuntime& rendering, ImVec2 displayOrigin, ImVec2 displaySize, float artPixel) {
    m_displayOrigin = displayOrigin;
    m_displaySize = displaySize;
    const float cell = cellSize(artPixel);
    const GridSize wanted{.width = static_cast<uint32_t>(std::ceil(std::max(displaySize.x, 1.0f) / cell)),
                          .height = static_cast<uint32_t>(std::ceil(std::max(displaySize.y, 1.0f) / cell)),
                          .cellSize = cell};
    m_stableFrames = wanted == m_wantedGrid ? m_stableFrames + 1 : 0;
    m_wantedGrid = wanted;

    if (m_generation.valid() && m_generation.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        publishGenerated(rendering);
    }
    const bool firstImage = !m_sharp && !m_generation.valid();
    const bool outdated = wanted != m_builtGrid && wanted != m_generatingGrid;
    if (!m_generation.valid() && outdated && (firstImage || m_stableFrames >= stableFramesBeforeRegenerating)) {
        startGeneration(wanted);
    }
    const UiTextureHandle sharp = rendering.uiImageTexture(m_sharp);
    // The size changes with the texture, not when the pixels are requested.
    if (sharp.value != m_sharpTexture.value) {
        m_imageSize = m_uploadedImageSize;
    }
    m_sharpTexture = sharp;
    m_frostedTexture = rendering.uiImageTexture(m_frosted);
}

void GlassBackdrop::release(RenderRuntime& rendering) {
    if (m_generation.valid()) {
        m_generation.wait();
        m_generation = {};
    }
    rendering.destroyUiImage(m_sharp);
    rendering.destroyUiImage(m_frosted);
    m_sharp = {};
    m_frosted = {};
    m_sharpTexture = {};
    m_frostedTexture = {};
    m_builtGrid = {};
    m_generatingGrid = {};
}

void GlassBackdrop::startGeneration(GridSize grid) {
    m_generatingGrid = grid;
    m_generation = std::async(std::launch::async, [grid] { return generateNightWallpaper(grid.width, grid.height); });
}

void GlassBackdrop::publishGenerated(RenderRuntime& rendering) {
    WallpaperImages images = m_generation.get();
    const GridSize grid = std::exchange(m_generatingGrid, {});
    UiImage sharp = toUiImage(std::move(images.sharp));
    UiImage frosted = toUiImage(std::move(images.frosted));
    // Updating keeps the previous textures on screen until the new ones upload.
    if (!rendering.updateUiImage(m_sharp, sharp)) {
        m_sharp = rendering.createUiImage(std::move(sharp));
    }
    if (!rendering.updateUiImage(m_frosted, frosted)) {
        m_frosted = rendering.createUiImage(std::move(frosted));
    }
    m_builtGrid = grid;
    m_uploadedImageSize = {static_cast<float>(grid.width) * grid.cellSize,
                           static_cast<float>(grid.height) * grid.cellSize};
}

void GlassBackdrop::drawWallpaper(ImDrawList* drawList) const {
    if (!m_sharpTexture) {
        // The sky's top and horizon colours until the wallpaper is ready.
        const ImU32 top = toImU32(palette::wallpaper[0]);
        const ImU32 bottom = toImU32(palette::wallpaper[4]);
        drawList->AddRectFilledMultiColor(m_displayOrigin,
                                          {m_displayOrigin.x + m_displaySize.x, m_displayOrigin.y + m_displaySize.y},
                                          top, top, bottom, bottom);
        return;
    }
    drawList->AddImage(static_cast<ImTextureID>(m_sharpTexture.value), m_displayOrigin,
                       {m_displayOrigin.x + m_imageSize.x, m_displayOrigin.y + m_imageSize.y});
}

void GlassBackdrop::drawFrosted(ImDrawList* drawList, ImVec2 min, ImVec2 max, float notch) const {
    min = pixel::snap(min);
    max = pixel::snap(max);
    if (!m_frostedTexture) {
        pixel::notchedRect(drawList, min, max, toImU32(palette::glassFallback), notch);
        return;
    }
    // The same cross of two rectangles as pixel::notchedRect, sampling the
    // frosted wallpaper where it lies on screen.
    drawImageRegion(drawList, m_frostedTexture, {min.x + notch, min.y}, {max.x - notch, max.y});
    drawImageRegion(drawList, m_frostedTexture, {min.x, min.y + notch}, {max.x, max.y - notch});
}

void GlassBackdrop::drawImageRegion(ImDrawList* drawList, UiTextureHandle texture, ImVec2 min, ImVec2 max) const {
    if (max.x <= min.x || max.y <= min.y || m_imageSize.x <= 0.0f || m_imageSize.y <= 0.0f) {
        return;
    }
    const ImVec2 uvMin{(min.x - m_displayOrigin.x) / m_imageSize.x, (min.y - m_displayOrigin.y) / m_imageSize.y};
    const ImVec2 uvMax{(max.x - m_displayOrigin.x) / m_imageSize.x, (max.y - m_displayOrigin.y) / m_imageSize.y};
    drawList->AddImage(static_cast<ImTextureID>(texture.value), min, max, uvMin, uvMax);
}
