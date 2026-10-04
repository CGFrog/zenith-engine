#pragma once

#include <imgui.h>

// Hard-edged drawing helpers for the pixel theme. Sizes are in screen
// pixels; callers pass the art pixel (editorArtPixel) where a shape needs
// one. Rectangles are snapped to whole screen pixels and anti-aliasing is
// off (applyEditorMetrics), so edges stay crisp.
namespace pixel {

// Rounds a point to whole screen pixels.
ImVec2 snap(ImVec2 point);

// A filled rectangle. Empty or inverted rectangles draw nothing.
void rect(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color);
// An outline of the given thickness just inside the rectangle.
void outline(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float thickness);
// A bevel one thickness wide inside the rectangle: highlight along the top
// and left edges, shade along the bottom and right.
void bevel(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 highlight, ImU32 shade, float thickness);
// A one-art-pixel outline just outside the rectangle: the focus ring.
void focusRing(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float artPixel);
// Text with a one-art-pixel outline drawn as four offset copies.
void outlinedText(ImDrawList* drawList, ImFont* font, float fontSize, ImVec2 position, ImU32 ink, ImU32 outline,
                  const char* text, float artPixel);
// A solid triangle filling the square of the given size at min, pointing
// in direction (the ▶ and ▼ markers).
void triangle(ImDrawList* drawList, ImVec2 min, float size, ImGuiDir direction, ImU32 color);
// A vertical line drawn one art pixel on, one off.
void dottedVerticalLine(ImDrawList* drawList, float x, float top, float bottom, ImU32 color, float artPixel);

} // namespace pixel
