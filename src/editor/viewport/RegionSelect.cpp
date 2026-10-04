#include "RegionSelect.hpp"

#include "editor/interaction/PickQueries.hpp"
#include "editor/theme/EditorPalette.hpp"
#include "editor/theme/EditorTheme.hpp"

#include <imgui.h>

#include <algorithm>

SelectionSetMode resolveSelectionSetMode(SelectionSetMode mode, bool shift, bool ctrl) {
    if (mode != SelectionSetMode::Auto) {
        return mode;
    }
    if (shift) {
        return SelectionSetMode::Extend;
    }
    if (ctrl) {
        return SelectionSetMode::Subtract;
    }
    return SelectionSetMode::Set;
}

void applySelectionSet(Selection& selection, std::span<const SceneEntityId> entities, SelectionSetMode mode) {
    const std::vector<SceneEntityId>& current = selection.ids();
    const auto enclosed = [&](SceneEntityId id) { return std::ranges::find(entities, id) != entities.end(); };
    std::vector<SceneEntityId> result;
    switch (mode) {
    case SelectionSetMode::Auto:
    case SelectionSetMode::Set: {
        const std::optional<SceneEntityId> active = selection.active();
        if (active && enclosed(*active)) {
            result.push_back(*active);
        }
        for (SceneEntityId id : entities) {
            if (std::ranges::find(result, id) == result.end()) {
                result.push_back(id);
            }
        }
        break;
    }
    case SelectionSetMode::Extend:
        result = current;
        for (SceneEntityId id : entities) {
            if (std::ranges::find(result, id) == result.end()) {
                result.push_back(id);
            }
        }
        break;
    case SelectionSetMode::Subtract:
        for (SceneEntityId id : current) {
            if (!enclosed(id)) {
                result.push_back(id);
            }
        }
        break;
    }
    selection.assign(std::move(result));
}

glm::vec2 regionMinimum(const RegionSelection& region) {
    glm::vec2 minimum = region.points.empty() ? glm::vec2(0.0f) : region.points.front();
    for (const glm::vec2& point : region.points) {
        minimum = glm::min(minimum, point);
    }
    return minimum;
}

glm::vec2 regionMaximum(const RegionSelection& region) {
    glm::vec2 maximum = region.points.empty() ? glm::vec2(0.0f) : region.points.front();
    for (const glm::vec2& point : region.points) {
        maximum = glm::max(maximum, point);
    }
    return maximum;
}

bool regionContains(const RegionSelection& region, glm::vec2 point) {
    if (region.shape == RegionShape::Lasso) {
        return insidePolygon(region.points, point);
    }
    const glm::vec2 minimum = regionMinimum(region);
    const glm::vec2 maximum = regionMaximum(region);
    return point.x >= minimum.x && point.y >= minimum.y && point.x <= maximum.x && point.y <= maximum.y;
}

void drawRegionOutline(ImDrawList& drawList, glm::vec2 origin, RegionShape shape, std::span<const glm::vec2> points) {
    if (points.empty()) {
        return;
    }
    const float thickness = std::max(editorArtPixel(), 1.0f);
    const ImU32 outline = toImU32(palette::guideLine);
    const auto screen = [origin](glm::vec2 point) { return ImVec2(origin.x + point.x, origin.y + point.y); };
    if (shape == RegionShape::Box) {
        const glm::vec2 first = points.front();
        const glm::vec2 last = points.back();
        const ImVec2 minimum = screen(glm::min(first, last));
        const ImVec2 maximum = screen(glm::max(first, last));
        drawList.AddRectFilled(minimum, maximum, toImU32(editorAccent().withAlpha(0.12f)));
        drawList.AddRect(minimum, maximum, outline, 0.0f, ImDrawFlags_None, thickness);
        return;
    }
    std::vector<ImVec2> screenPoints;
    screenPoints.reserve(points.size());
    for (const glm::vec2& point : points) {
        screenPoints.push_back(screen(point));
    }
    drawList.AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()), outline, ImDrawFlags_Closed,
                         thickness);
}
