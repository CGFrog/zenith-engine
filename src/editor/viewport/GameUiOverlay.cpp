#include "GameUiOverlay.hpp"

#include "engine/ui/GameUi.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <variant>
#include <vector>

namespace {
ImU32 color(const glm::vec4& value) {
    return ImGui::ColorConvertFloat4ToU32({value.r, value.g, value.b, value.a});
}

ImVec2 point(glm::vec2 origin, glm::vec2 position) {
    return {std::floor(origin.x + position.x), std::floor(origin.y + position.y)};
}

void drawRect(ImDrawList& drawList, glm::vec2 origin, const UiRect& rect, ImU32 fill, float cornerRadius) {
    drawList.AddRectFilled(point(origin, rect.position), point(origin, rect.position + rect.size), fill, cornerRadius);
}
} // namespace

void drawGameUiOverlay(ImDrawList& drawList, glm::vec2 origin, const GameUiContext& ui) {
    std::vector<const UiCommand*> ordered;
    ordered.reserve(ui.commands().size());
    for (const UiCommand& command : ui.commands()) {
        ordered.push_back(&command);
    }
    // Stable, so equal draw orders keep recording order, as in the engine.
    std::ranges::stable_sort(ordered, {}, &UiCommand::drawOrder);
    for (const UiCommand* command : ordered) {
        if (command->clip) {
            drawList.PushClipRect(point(origin, command->clip->position),
                                  point(origin, command->clip->position + command->clip->size), true);
        }
        if (const auto* panel = std::get_if<UiPanelCommand>(&command->payload)) {
            drawRect(drawList, origin, panel->rect, color(panel->style.color), panel->style.cornerRadius);
        } else if (const auto* text = std::get_if<UiTextCommand>(&command->payload)) {
            drawList.AddText(nullptr, text->size, point(origin, text->position), color(text->color), text->text.c_str(),
                             text->text.c_str() + text->text.size());
        } else if (const auto* image = std::get_if<UiImageCommand>(&command->payload)) {
            drawRect(drawList, origin, image->rect, color(image->tint), 0.0f);
        }
        if (command->clip) {
            drawList.PopClipRect();
        }
    }
}
