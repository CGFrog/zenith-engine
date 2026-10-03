#include "editor/theme/PixelIcons.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <algorithm>
#include <cmath>

float iconSize(float artPixel) {
    return static_cast<float>(iconBitmapSize) * artPixel;
}

void drawIcon(ImDrawList* drawList, EditorIcon icon, ImVec2 topLeft, float pixelSize, IconState state) {
    const IconBitmap& bitmap = iconBitmap(icon);
    const ImVec2 origin = pixel::snap(topLeft);
    const float size = std::max(1.0f, std::floor(pixelSize));
    for (size_t row = 0; row < bitmap.rows.size(); ++row) {
        const std::string_view pixels = bitmap.rows[row];
        size_t column = 0;
        while (column < pixels.size()) {
            const char value = pixels[column];
            size_t end = column + 1;
            // A disabled icon draws single pixels on a checkerboard instead of runs.
            if (!state.disabled) {
                while (end < pixels.size() && pixels[end] == value) {
                    ++end;
                }
            }
            const bool skipped = state.disabled && (row + column) % 2u == 1u;
            if (const std::optional<ThemeColor> color = iconPixelColor(bitmap, value); color && !skipped) {
                const ImVec2 min{origin.x + static_cast<float>(column) * size,
                                 origin.y + static_cast<float>(row) * size};
                drawList->AddRectFilled(min, {min.x + static_cast<float>(end - column) * size, min.y + size},
                                        toImU32(*color));
            }
            column = end;
        }
    }
}
