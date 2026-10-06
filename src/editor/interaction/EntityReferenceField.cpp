#include "EntityReferenceField.hpp"

#include "editor/interaction/EntityEyedropper.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace {
ThemeColor stateInk(EntityReferenceState state) {
    switch (state) {
    case EntityReferenceState::None:
        return palette::inkMuted;
    case EntityReferenceState::Present:
        return palette::ink;
    case EntityReferenceState::Missing:
        return palette::error;
    }
    return palette::ink;
}

// The entity dragged from the outliner onto the last item, once dropped.
std::optional<SceneEntityId> acceptEntityDrop() {
    std::optional<SceneEntityId> dropped;
    if (!ImGui::BeginDragDropTarget()) {
        return dropped;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(sceneEntityDragPayload);
        payload != nullptr && payload->DataSize == static_cast<int>(sizeof(uint64_t))) {
        uint64_t id = 0;
        std::memcpy(&id, payload->Data, sizeof(id));
        dropped = SceneEntityId{id};
    }
    ImGui::EndDragDropTarget();
    return dropped;
}

// A recessed box naming the referenced entity; returns a dropped entity.
std::optional<SceneEntityId> drawValueBox(float width, SceneEntityId value, const Scene& scene, bool acceptsDrop) {
    ImGui::InvisibleButton("##value", {width, ImGui::GetFrameHeight()});
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, widgets::color(hovered ? palette::fieldHovered : palette::field));
    const EntityReferenceState state = entityReferenceState(scene, value);
    const std::string text = entityReferenceText(scene, value);
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    drawList->PushClipRect({min.x + padding.x, min.y}, {max.x - padding.x, max.y}, true);
    drawList->AddText({min.x + padding.x, min.y + padding.y}, widgets::color(stateInk(state)), text.c_str());
    drawList->PopClipRect();
    if (hovered) {
        if (state == EntityReferenceState::None) {
            ImGui::SetTooltip("No entity. Drop one from the outliner here.");
        } else {
            ImGui::SetTooltip("%s (ID %llu)%s", text.c_str(), static_cast<unsigned long long>(value.value),
                              state == EntityReferenceState::Missing ? "\nNo entity in the scene has this ID." : "");
        }
    }
    return acceptsDrop ? acceptEntityDrop() : std::nullopt;
}

void drawPickButton(const FieldDescriptor& field, const EntityReferenceFieldContext& context) {
    std::optional<std::vector<std::string>> path = findFieldPath(context.componentFields, field);
    if (!path) {
        return;
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    EntityReferenceTarget target{
        .entity = context.owner, .componentKey = std::string(context.componentKey), .fieldPath = std::move(*path)};
    const EntityReferenceTarget* armed = context.eyedropper->target();
    const bool picking = armed != nullptr && context.eyedropper->owner() == context.document && *armed == target;
    if (widgets::Button("Pick", {0.0f, 0.0f}, picking)) {
        if (picking) {
            context.cancel();
        } else {
            context.arm(target);
        }
    }
    ImGui::SetItemTooltip("%s", picking ? "Click an entity in a viewport or the outliner. Esc or right-click cancels."
                                        : "Pick the entity from a viewport or the outliner");
}
} // namespace

EntityReferenceFieldContext entityReferenceFieldContext(EditorContext& context, SceneEntityId owner,
                                                        const ComponentType& type) {
    return {.scene = context.app.scene,
            .eyedropper = context.eyedropper,
            .document = &context.editing.document(),
            .owner = owner,
            .componentKey = type.key(),
            .componentFields = type.fields(),
            .arm =
                [&context](const EntityReferenceTarget& target) {
                    context.invoke("ui.eyedropper_entity", nlohmann::json{{"entity", target.entity.value},
                                                                          {"component", target.componentKey},
                                                                          {"field", joinFieldPath(target.fieldPath)}}
                                                               .dump());
                },
            .cancel = [&context] { context.invoke("ui.eyedropper_cancel"); }};
}

EntityReferenceState entityReferenceState(const Scene& scene, SceneEntityId value) {
    if (!value) {
        return EntityReferenceState::None;
    }
    return scene.findEntity(value) == entt::null ? EntityReferenceState::Missing : EntityReferenceState::Present;
}

std::string entityReferenceText(const Scene& scene, SceneEntityId value) {
    switch (entityReferenceState(scene, value)) {
    case EntityReferenceState::None:
        return "None";
    case EntityReferenceState::Missing:
        return "Missing";
    case EntityReferenceState::Present:
        break;
    }
    const auto* name = scene.registry().try_get<NameComponent>(scene.findEntity(value));
    return name == nullptr || name->name.empty() ? std::string("Unnamed Entity") : name->name;
}

bool drawEntityReferenceField(const char* label, SceneEntityId& value, const FieldDescriptor& field,
                              const EntityReferenceFieldContext* context) {
    if (context == nullptr) {
        ImGui::Text("%s: ID %llu", label, static_cast<unsigned long long>(value.value));
        return false;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemInnerSpacing.x;
    const bool offersPick = context->eyedropper != nullptr && context->arm && context->cancel;
    float buttonsWidth = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f + spacing;
    if (offersPick) {
        buttonsWidth += ImGui::CalcTextSize("Pick").x + style.FramePadding.x * 2.0f + spacing;
    }
    const float valueWidth = std::max(ImGui::CalcItemWidth() - buttonsWidth, ImGui::GetFrameHeight());

    bool changed = false;
    if (const std::optional<SceneEntityId> dropped =
            drawValueBox(valueWidth, value, context->scene, !field.presentation.readOnly);
        dropped && *dropped != value) {
        value = *dropped;
        changed = true;
    }
    ImGui::SameLine(0.0f, spacing);
    ImGui::BeginDisabled(!value);
    if (widgets::Button("Clear")) {
        value = {};
        changed = true;
    }
    ImGui::EndDisabled();
    if (offersPick) {
        drawPickButton(field, *context);
    }
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    return changed;
}
