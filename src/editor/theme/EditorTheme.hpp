#pragma once

#include "editor/theme/EditorPalette.hpp"
#include "editor/theme/ThemeParameters.hpp"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Presentation state of the editor's "8-bit Blender" theme. Like ImGui's own
// style it is global: widgets anywhere read the art-pixel size and the accent
// without the theme being threaded through every call.

// The theme's pixel grid: one art pixel in screen pixels, a whole number so
// pixel art and hard edges stay crisp. Display scales round to the nearest.
float artPixelForScale(float contentScale);
// Screen pixels per art pixel for a UI scale setting: an override (whole
// multipliers from 1 to maximumUiScale) or, when 0, the display's scale.
float artPixelForUiScale(uint32_t uiScaleOverride, float contentScale);
// The art pixel the current metrics were built for (1 before any are applied).
float editorArtPixel();

// Sizes, spacing and borders on the art-pixel grid, with every rounding and
// anti-aliasing switched off.
void applyEditorMetrics(ImGuiStyle& style, float artPixel);
// Every ImGui and ImGuizmo colour from the palette, with the parameters'
// accent. The accent is also kept in the style (as its
// NavCursor colour), where editorAccent reads it.
void applyEditorColors(ImGuiStyle& style, const ThemeParameters& parameters = {});
// The accent of the current ImGui style.
ThemeColor editorAccent();

ImVec4 toImVec4(ThemeColor color);
ImU32 toImU32(ThemeColor color);

// The font loaded for one art pixel size: the pixel font, or the fallback
// without it.
struct EditorFonts {
    float artPixel{0.0f};
    ImFont* font{nullptr};
};

struct EditorFontResult {
    bool success{false};
    std::string error;
    // Optional fonts that were missing; the editor still runs without them.
    std::vector<std::string> warnings;
    EditorFonts fonts;

    explicit operator bool() const {
        return success;
    }
};

// Loads the pixel font (VT323) from fontDirectory as the ImGui default, with
// the fallback font merged in for glyphs it lacks; titles use the same font
// and stand out by colour. Without the pixel font the fallback alone is the
// default. Must run before the first UI frame so the backend uploads the
// atlas with these fonts; fonts added later (another UI scale) must be added
// between frames, and the renderer uploads the grown atlas at the next frame.
EditorFontResult loadEditorFonts(ImGuiIO& io, const std::filesystem::path& fontDirectory,
                                 const std::filesystem::path& fallbackFont, float artPixel);
// Makes a font loaded earlier the default again.
void useEditorFonts(ImGuiIO& io, const EditorFonts& fonts);
