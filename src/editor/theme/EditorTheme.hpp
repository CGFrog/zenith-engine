#pragma once

#include "editor/theme/EditorPalette.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

// Presentation state of the editor's "Pixel Aero Night" theme. Like ImGui's
// own style it is global: widgets anywhere read the art-pixel size and the
// title font without the theme being threaded through every call.

// The theme's pixel grid: one art pixel in screen pixels, a whole number so
// pixel art and hard edges stay crisp. Display scales round to the nearest.
float artPixelForScale(float contentScale);
// The art pixel the current metrics were built for (1 before any are applied).
float editorArtPixel();

// Sizes, spacing and borders on the art-pixel grid, with every rounding and
// anti-aliasing switched off.
void applyEditorMetrics(ImGuiStyle& style, float artPixel);
// Every ImGui and ImGuizmo colour from the palette.
void applyEditorColors(ImGuiStyle& style);

ImVec4 toImVec4(ThemeColor color);
ImU32 toImU32(ThemeColor color);

struct EditorFontResult {
    bool success{false};
    std::string error;
    // Optional fonts that were missing; the editor still runs without them.
    std::vector<std::string> warnings;

    explicit operator bool() const {
        return success;
    }
};

// Loads the pixel fonts from fontDirectory: VT323 for body text (the ImGui
// default) and Press Start 2P for titles, each with the fallback font merged
// in for glyphs the pixel fonts lack. Without the pixel fonts the fallback
// alone is the default. Must run before the first UI frame so the backend
// uploads the atlas with these fonts.
EditorFontResult loadEditorFonts(ImGuiIO& io, const std::filesystem::path& fontDirectory,
                                 const std::filesystem::path& fallbackFont, float artPixel);
// The title font, or null to use the current font (before fonts load or
// without the pixel fonts).
ImFont* editorTitleFont();
