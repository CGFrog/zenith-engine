#include "editor/theme/PixelPainter.hpp"

#include <algorithm>
#include <cmath>

namespace pixel {
namespace {
bool empty(ImVec2 min, ImVec2 max) {
    return max.x <= min.x || max.y <= min.y;
}

// The largest notch that still leaves a body between opposite corners.
float clampNotch(ImVec2 min, ImVec2 max, float notch) {
    return std::clamp(std::floor(notch), 0.0f, std::floor(std::min(max.x - min.x, max.y - min.y) / 2.0f));
}
} // namespace

ImVec2 snap(ImVec2 point) {
    return {std::round(point.x), std::round(point.y)};
}

void notchedRect(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float notch) {
    min = snap(min);
    max = snap(max);
    if (empty(min, max)) {
        return;
    }
    const float cut = clampNotch(min, max, notch);
    // A cross of two rectangles leaves the corner cells empty.
    drawList->AddRectFilled({min.x + cut, min.y}, {max.x - cut, max.y}, color);
    if (cut > 0.0f) {
        drawList->AddRectFilled({min.x, min.y + cut}, {min.x + cut, max.y - cut}, color);
        drawList->AddRectFilled({max.x - cut, min.y + cut}, {max.x, max.y - cut}, color);
    }
}

void splitGloss(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 top, ImU32 bottom, float notch) {
    min = snap(min);
    max = snap(max);
    if (empty(min, max)) {
        return;
    }
    notchedRect(drawList, min, max, bottom, notch);
    const float middle = std::floor((min.y + max.y) / 2.0f);
    const float cut = clampNotch(min, max, notch);
    drawList->AddRectFilled({min.x + cut, min.y}, {max.x - cut, middle}, top);
    if (middle > min.y + cut) {
        drawList->AddRectFilled({min.x, min.y + cut}, {max.x, middle}, top);
    }
}

void notchedOutline(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float thickness) {
    min = snap(min);
    max = snap(max);
    if (empty(min, max) || thickness <= 0.0f) {
        return;
    }
    const float cut = clampNotch(min, max, thickness);
    // Edges stop short of the corners; one cell per corner steps inward.
    drawList->AddRectFilled({min.x + cut, min.y}, {max.x - cut, min.y + thickness}, color);
    drawList->AddRectFilled({min.x + cut, max.y - thickness}, {max.x - cut, max.y}, color);
    drawList->AddRectFilled({min.x, min.y + cut}, {min.x + thickness, max.y - cut}, color);
    drawList->AddRectFilled({max.x - thickness, min.y + cut}, {max.x, max.y - cut}, color);
    drawList->AddRectFilled({min.x + cut, min.y + cut}, {min.x + cut + thickness, min.y + cut + thickness}, color);
    drawList->AddRectFilled({max.x - cut - thickness, min.y + cut}, {max.x - cut, min.y + cut + thickness}, color);
    drawList->AddRectFilled({min.x + cut, max.y - cut - thickness}, {min.x + cut + thickness, max.y - cut}, color);
    drawList->AddRectFilled({max.x - cut - thickness, max.y - cut - thickness}, {max.x - cut, max.y - cut}, color);
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

void glowRing(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color, float artPixel) {
    notchedOutline(drawList, {min.x - artPixel, min.y - artPixel}, {max.x + artPixel, max.y + artPixel}, color,
                   artPixel);
}

void glintStripes(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 strong, ImU32 faint, float artPixel) {
    min = snap(min);
    max = snap(max);
    if (empty(min, max)) {
        return;
    }
    const float height = max.y - min.y;
    // Stripes lean about 30 degrees from vertical, a fifth of the way across.
    const float lean = height * 0.58f;
    const float start = min.x + std::floor((max.x - min.x) * 0.2f);
    const auto stripe = [&](float offset, float width, ImU32 color) {
        const float left = start + offset;
        drawList->AddQuadFilled({left + lean, min.y}, {left + lean + width, min.y}, {left + width, max.y},
                                {left, max.y}, color);
    };
    drawList->PushClipRect(min, max, true);
    stripe(0.0f, artPixel * 3.0f, strong);
    stripe(artPixel * 6.0f, artPixel * 2.0f, faint);
    drawList->PopClipRect();
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
