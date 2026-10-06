#pragma once

// Draws the pixel-art icons at the current art-pixel scale.

#include "editor/theme/PixelIconBitmaps.hpp"

#include <imgui.h>

struct IconState {
    // Drawn as a dithered ghost: every other pixel.
    bool disabled{false};
};

// Icon edge in screen pixels for one art pixel per bitmap pixel.
float iconSize(float artPixel);
// Draws the icon with its top-left corner at topLeft, each bitmap pixel
// pixelSize screen pixels square (the art pixel, or a multiple for large
// tiles). Rows are drawn as merged runs of equal colour.
void drawIcon(ImDrawList* drawList, EditorIcon icon, ImVec2 topLeft, float pixelSize, IconState state = {});
