#include "LastOperatorPanel.hpp"

#include "editor/interaction/ReflectedFieldEditor.hpp"

#include <imgui.h>

std::optional<OperatorProperties> LastOperatorPanel::draw(const LastOperator* last, glm::vec2 anchor) {
    if (last == nullptr || last->properties.empty()) {
        return std::nullopt;
    }
    constexpr float margin = 8.0f;
    ImGui::SetNextWindowPos({anchor.x + margin, anchor.y - margin}, ImGuiCond_Always, {0.0f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.9f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoSavedSettings;
    std::optional<OperatorProperties> edited;
    // The label is the title; the id keeps the window (and its collapsed
    // state) the same across operators.
    const std::string title = last->label + "###AdjustLastOperation";
    if (ImGui::Begin(title.c_str(), nullptr, flags)) {
        OperatorProperties values = last->properties;
        ImGui::PushItemWidth(ImGui::GetFontSize() * 14.0f);
        if (drawReflectedFields(values.object(), values.fields())) {
            edited = std::move(values);
        }
        ImGui::PopItemWidth();
    }
    ImGui::End();
    return edited;
}
