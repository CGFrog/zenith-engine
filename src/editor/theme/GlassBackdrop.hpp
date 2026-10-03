#pragma once

#include "editor/theme/PixelWallpaper.hpp"
#include "editor/theme/ThemeParameters.hpp"

#include "engine/graphics/Rendering.hpp"

#include <imgui.h>

#include <cstdint>
#include <future>

// The wallpaper behind the editor and the frosted glass drawn over it. The
// glass samples a pre-blurred copy of the wallpaper at the same screen
// position, so it reads as frosted glass without a blur pass. Until the
// renderer publishes the textures (always, without a renderer), and while the
// theme's wallpaper is off, both draw solid fallbacks.
class GlassBackdrop {
  public:
    GlassBackdrop() = default;
    GlassBackdrop(const GlassBackdrop&) = delete;
    GlassBackdrop& operator=(const GlassBackdrop&) = delete;
    ~GlassBackdrop();

    // Wallpaper cell edge in screen pixels.
    static float cellSize(float artPixel) {
        return 3.0f * artPixel;
    }

    // Once per frame: keeps the wallpaper matching the display and the
    // theme's frost. A new size or frost is generated on a worker thread after
    // it has held for a few frames, so a window drag or a slider being dragged
    // does not regenerate every frame.
    void update(RenderRuntime& rendering, ImVec2 displayOrigin, ImVec2 displaySize, float artPixel,
                const ThemeParameters& parameters = {});
    // Destroys the wallpaper images.
    void release(RenderRuntime& rendering);

    bool texturesReady() const {
        return static_cast<bool>(m_sharpTexture) && static_cast<bool>(m_frostedTexture);
    }

    // Whether the wallpaper images are drawn: their textures exist and the
    // theme shows the wallpaper.
    bool wallpaperShown() const {
        return m_wallpaper && texturesReady();
    }

    // The sharp wallpaper over the whole display.
    void drawWallpaper(ImDrawList* drawList) const;
    // Frosted glass over a rectangle with notched corners.
    void drawFrosted(ImDrawList* drawList, ImVec2 min, ImVec2 max, float notch) const;

  private:
    // What one generation produces: the grid and the frosted copy's look.
    struct GridSize {
        uint32_t width{0};
        uint32_t height{0};
        float cellSize{0.0f};
        uint32_t frostRadius{0};
        float glassOpacity{0.0f};

        bool operator==(const GridSize&) const = default;
    };

    void startGeneration(GridSize grid);
    void publishGenerated(RenderRuntime& rendering);
    void drawImageRegion(ImDrawList* drawList, UiTextureHandle texture, ImVec2 min, ImVec2 max) const;

    UiImageId m_sharp;
    UiImageId m_frosted;
    UiTextureHandle m_sharpTexture;
    UiTextureHandle m_frostedTexture;
    // The grid the uploaded images cover, and their size on screen; they are
    // anchored at the display origin.
    GridSize m_builtGrid;
    ImVec2 m_imageSize{0.0f, 0.0f};
    // The size of the requested images, applied once their texture appears.
    ImVec2 m_uploadedImageSize{0.0f, 0.0f};
    ImVec2 m_displayOrigin{0.0f, 0.0f};
    ImVec2 m_displaySize{0.0f, 0.0f};
    // The grid the display needs and how many frames it has been stable.
    GridSize m_wantedGrid;
    uint32_t m_stableFrames{0};
    GridSize m_generatingGrid;
    std::future<WallpaperImages> m_generation;
    bool m_wallpaper{true};
};
