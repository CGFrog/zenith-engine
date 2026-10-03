#include "OutlinerRowWidgets.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/PixelIcons.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <cmath>

namespace {
float toggleWidth() {
    return iconSize(editorArtPixel()) + editorArtPixel() * 4.0f;
}
} // namespace

void decorateOutlinerRow(EditorIcon icon, bool selected, bool leaf, bool ghost) {
    using namespace palette;
    const float artPixel = editorArtPixel();
    const float labelOffset = ImGui::GetTreeNodeToLabelSpacing();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (selected) {
        drawList->AddRectFilled(min, {max.x, min.y + artPixel}, widgets::color(rowSelectedHighlight));
        drawList->AddRectFilled({min.x, max.y - artPixel}, max, widgets::color(rowSelectedShade));
        if (leaf) {
            const float cursor = artPixel * 6.0f;
            pixel::triangle(drawList, {min.x + (labelOffset - cursor) / 2.0f, (min.y + max.y - cursor) / 2.0f}, cursor,
                            ImGuiDir_Right, widgets::color(pixelGlow));
        }
    }
    widgets::ItemIcon(icon, labelOffset, {.disabled = ghost});
}

float restrictionColumnsWidth(size_t count) {
    return toggleWidth() * static_cast<float>(count);
}

int drawRestrictionToggles(ImVec2 rowMin, ImVec2 rowMax, std::span<const RestrictionToggle> toggles) {
    const float width = toggleWidth();
    const float artPixel = editorArtPixel();
    const float extent = iconSize(artPixel);
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    float x = right - restrictionColumnsWidth(toggles.size());
    int clicked = -1;
    for (size_t index = 0; index < toggles.size(); ++index) {
        const RestrictionToggle& toggle = toggles[index];
        const ImVec2 topLeft{std::floor(x), rowMin.y};
        const ImVec2 size{width, rowMax.y - rowMin.y};
        ImGui::SetCursorScreenPos(topLeft);
        if (ImGui::InvisibleButton(toggle.id, size)) {
            clicked = static_cast<int>(index);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::GetWindowDrawList()->AddRectFilled(topLeft, {topLeft.x + size.x, topLeft.y + size.y},
                                                      widgets::color(palette::rowHovered));
            ImGui::SetTooltip("%s", toggle.tooltip);
        }
        drawIcon(ImGui::GetWindowDrawList(), toggle.icon,
                 {std::floor(topLeft.x + (size.x - extent) / 2.0f), std::floor(topLeft.y + (size.y - extent) / 2.0f)},
                 artPixel, {.disabled = !toggle.enabled});
        x += width;
    }
    return clicked;
}
