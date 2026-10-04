#include "editor/theme/PixelPainter.hpp"

#include <algorithm>
#include <cmath>

namespace pixel {
namespace {
bool empty(ImVec2 min, ImVec2 max) {
    return max.x <= min.x || max.y <= min.y;
}
} // namespace

ImVec2 snap(ImVec2 point) {
    return {std::round(point.x), std::round(point.y)};
}

void rect(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color) {
    min = snap(min);
    max = snap(max);
    if (empty(min, max)) {
        return;
    }
    drawList->AddRectFilled(min, max, color);
}

void outline(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float thickness) {
    min = snap(min);
    max = snap(max);
    if (empty(min, max) || thickness <= 0.0f) {
        return;
    }
    // Four strips; the sides stop short of the top and bottom so no pixel is
    // drawn twice (translucent colours would double up).
    const float side = std::min(thickness, std::floor((max.y - min.y) / 2.0f));
    drawList->AddRectFilled(min, {max.x, min.y + side}, color);
    drawList->AddRectFilled({min.x, max.y - side}, max, color);
    drawList->AddRectFilled({min.x, min.y + side}, {min.x + thickness, max.y - side}, color);
    drawList->AddRectFilled({max.x - thickness, min.y + side}, {max.x, max.y - side}, color);
}

void bevel(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 highlight, ImU32 shade, float thickness) {
    min = snap(min);
    max = snap(max);
    if (empty(min, max) || thickness <= 0.0f) {
        return;
    }
    drawList->AddRectFilled(min, {max.x - thickness, min.y + thickness}, highlight);
    drawList->AddRectFilled({min.x, min.y + thickness}, {min.x + thickness, max.y - thickness}, highlight);
    drawList->AddRectFilled({min.x + thickness, max.y - thickness}, max, shade);
    drawList->AddRectFilled({max.x - thickness, min.y + thickness}, {max.x, max.y - thickness}, shade);
}

void focusRing(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float artPixel) {
    outline(drawList, {min.x - artPixel, min.y - artPixel}, {max.x + artPixel, max.y + artPixel}, color, artPixel);
}

void outlinedText(ImDrawList* drawList, ImFont* font, float fontSize, ImVec2 position, ImU32 ink, ImU32 outline,
                  const char* text, float artPixel) {
    position = snap(position);
    for (const ImVec2 offset :
         {ImVec2{-artPixel, 0.0f}, ImVec2{artPixel, 0.0f}, ImVec2{0.0f, -artPixel}, ImVec2{0.0f, artPixel}}) {
        drawList->AddText(font, fontSize, {position.x + offset.x, position.y + offset.y}, outline, text);
    }
    drawList->AddText(font, fontSize, position, ink, text);
}

void triangle(ImDrawList* drawList, ImVec2 min, float size, ImGuiDir direction, ImU32 color) {
    min = snap(min);
    const float extent = std::floor(size);
    const float half = std::floor(extent / 2.0f);
    if (direction == ImGuiDir_Down) {
        drawList->AddTriangleFilled({min.x, min.y + half / 2.0f}, {min.x + extent, min.y + half / 2.0f},
                                    {min.x + half, min.y + half / 2.0f + half}, color);
        return;
    }
    drawList->AddTriangleFilled({min.x + half / 2.0f, min.y}, {min.x + half / 2.0f, min.y + extent},
                                {min.x + half / 2.0f + half, min.y + half}, color);
}

void dottedVerticalLine(ImDrawList* drawList, float x, float top, float bottom, ImU32 color, float artPixel) {
    x = std::round(x);
    for (float y = std::round(top); y < bottom; y += artPixel * 2.0f) {
        drawList->AddRectFilled({x, y}, {x + artPixel, std::min(y + artPixel, bottom)}, color);
    }
}

} // namespace pixel
