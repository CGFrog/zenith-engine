#include "ViewportChrome.hpp"

#include "editor/interaction/EditorCamera.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

#include <glm/trigonometric.hpp>

#include <array>
#include <utility>

namespace {
struct ShadingProperties {
    ViewportShading type{ViewportShading::Rendered};
};

constexpr std::array<ViewportShading, 4> shadingModes{ViewportShading::Rendered, ViewportShading::MaterialPreview,
                                                      ViewportShading::Solid, ViewportShading::Wireframe};

struct ToolChoice {
    const char* label;
    const char* description;
    GizmoMode mode;
    EditorIcon icon;
};

constexpr std::array<ToolChoice, 4> toolChoices{{
    {"Tweak", "Select objects by clicking; no gizmo", GizmoMode::Select, EditorIcon::ToolTweak},
    {"Move", "Move the selection with a gizmo", GizmoMode::Translate, EditorIcon::ToolMove},
    {"Rotate", "Rotate the selection with a gizmo", GizmoMode::Rotate, EditorIcon::ToolRotate},
    {"Scale", "Scale the selection with a gizmo", GizmoMode::Scale, EditorIcon::ToolScale},
}};

constexpr const char* noViewportError = "No viewport is open";

std::function<bool(const OperatorContext&)> hasTarget(const ViewportChromeTarget& target) {
    return [target](const OperatorContext&) { return target() != nullptr; };
}

Operator toggleOperator(std::string id, std::string label, std::string description, ViewportChromeTarget target,
                        bool ViewportChromeState::* flag) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .poll = hasTarget(target),
            .execute = [target, flag](OperatorContext&, OperatorProperties&) {
                ViewportChromeState* state = target();
                if (state == nullptr) {
                    return OperatorResult::failed(noViewportError);
                }
                state->*flag = !(state->*flag);
                return OperatorResult::finished();
            }};
}

void drawSpaceChoice(TransformToolSettings& tools) {
    int space = static_cast<int>(tools.space);
    ImGui::TextUnformatted("Orientation");
    ImGui::RadioButton("Global", &space, static_cast<int>(TransformSpace::Global));
    ImGui::SameLine();
    ImGui::RadioButton("Local", &space, static_cast<int>(TransformSpace::Local));
    tools.space = static_cast<TransformSpace>(space);
}

void drawSnapIncrements(TransformToolSettings& tools) {
    // Scoped so the labels do not collide with the tool choices of the same name.
    ImGui::PushID("SnapIncrements");
    ImGui::DragFloat("Move", &tools.translationSnap, 0.05f, 0.001f, 1000.0f, "%.3f m");
    ImGui::DragFloat("Rotate", &tools.rotationSnapDegrees, 0.5f, 0.1f, 180.0f, "%.1f deg");
    ImGui::DragFloat("Scale", &tools.scaleSnap, 0.01f, 0.001f, 100.0f, "%.3f");
    ImGui::PopID();
}
} // namespace

float& TransformToolSettings::activeSnap() {
    if (gizmo == GizmoMode::Translate) {
        return translationSnap;
    }
    return gizmo == GizmoMode::Rotate ? rotationSnapDegrees : scaleSnap;
}

const char* viewportShadingLabel(ViewportShading shading) {
    switch (shading) {
    case ViewportShading::Rendered:
        return "Rendered";
    case ViewportShading::MaterialPreview:
        return "Material Preview";
    case ViewportShading::Solid:
        return "Solid";
    case ViewportShading::Wireframe:
        return "Wireframe";
    }
    return "Rendered";
}

std::vector<Operator> viewportChromeOperators(ViewportChromeTarget target) {
    std::vector<Operator> operators;
    operators.push_back(toggleOperator("view3d.toggle_sidebar", "Toggle Sidebar",
                                       "Show or hide the viewport's Item, Tool, and View sidebar", target,
                                       &ViewportChromeState::sidebarVisible));
    operators.push_back(toggleOperator("view3d.toggle_toolbar", "Toggle Toolbar",
                                       "Show or hide the viewport's tool shelf", target,
                                       &ViewportChromeState::toolbarVisible));
    operators.push_back(
        {.id = "view3d.shading_set",
         .label = "Set Shading",
         .description = "Choose how the viewport shades the scene",
         .properties = OperatorProperties::make(
             ShadingProperties{},
             std::move(Fields<ShadingProperties>()
                           .add("type", &ShadingProperties::type)
                           .options({{"rendered", "Rendered", static_cast<int64_t>(ViewportShading::Rendered)},
                                     {"material_preview", "Material Preview",
                                      static_cast<int64_t>(ViewportShading::MaterialPreview)},
                                     {"solid", "Solid", static_cast<int64_t>(ViewportShading::Solid)},
                                     {"wireframe", "Wireframe", static_cast<int64_t>(ViewportShading::Wireframe)}}))),
         .poll = hasTarget(target),
         .execute = [target](OperatorContext&, OperatorProperties& properties) {
             ViewportChromeState* state = target();
             if (state == nullptr) {
                 return OperatorResult::failed(noViewportError);
             }
             const ShadingProperties* values = properties.get<ShadingProperties>();
             state->shading = values != nullptr ? values->type : ViewportShading::Rendered;
             return OperatorResult::finished();
         }});
    return operators;
}

namespace ViewportChromeWidgets {
void drawShadingMenu(ViewportChromeState& state) {
    for (const ViewportShading mode : shadingModes) {
        if (ImGui::MenuItem(viewportShadingLabel(mode), nullptr, state.shading == mode)) {
            state.shading = mode;
        }
    }
}

void drawOverlaysMenu(ViewportOverlays& overlays) {
    ImGui::MenuItem("Grid", nullptr, &overlays.grid);
    ImGui::MenuItem("Selection Outline", nullptr, &overlays.selectionOutline);
    ImGui::MenuItem("Active Object Name", nullptr, &overlays.activeName);
    ImGui::MenuItem("Transform Gizmo", nullptr, &overlays.gizmo);
}

void drawGizmosMenu(TransformToolSettings& tools) {
    if (ImGui::MenuItem("Global", nullptr, tools.space == TransformSpace::Global)) {
        tools.space = TransformSpace::Global;
    }
    if (ImGui::MenuItem("Local", nullptr, tools.space == TransformSpace::Local)) {
        tools.space = TransformSpace::Local;
    }
    ImGui::Separator();
    ImGui::MenuItem("Snapping", nullptr, &tools.snapping);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 8.0f);
    drawSnapIncrements(tools);
    ImGui::PopItemWidth();
}

void drawToolShelf(TransformToolSettings& tools, float buttonWidth) {
    for (const ToolChoice& choice : toolChoices) {
        if (widgets::ToolButton(choice.label, choice.icon, tools.gizmo == choice.mode, {buttonWidth, buttonWidth})) {
            tools.gizmo = choice.mode;
        }
        ImGui::SetItemTooltip("%s: %s", choice.label, choice.description);
    }
}

void drawToolTab(TransformToolSettings& tools) {
    int tool = static_cast<int>(tools.gizmo);
    ImGui::TextUnformatted("Active Tool");
    for (const ToolChoice& choice : toolChoices) {
        ImGui::RadioButton(choice.label, &tool, static_cast<int>(choice.mode));
    }
    tools.gizmo = static_cast<GizmoMode>(tool);
    ImGui::Separator();
    drawSpaceChoice(tools);
    ImGui::Separator();
    ImGui::Checkbox("Snapping", &tools.snapping);
    drawSnapIncrements(tools);
}

void drawViewTab(EditorCamera& camera) {
    float fovDegrees = glm::degrees(camera.verticalFovRadians());
    if (ImGui::SliderFloat("Field of View", &fovDegrees, 5.0f, 150.0f, "%.1f deg")) {
        camera.setVerticalFovRadians(glm::radians(fovDegrees));
    }
    float clipStart = camera.nearPlane();
    float clipEnd = camera.farPlane();
    const bool clipStartChanged = ImGui::DragFloat("Clip Start", &clipStart, 0.005f, 0.0001f, 100.0f, "%.4f m");
    const bool clipEndChanged = ImGui::DragFloat("Clip End", &clipEnd, 1.0f, 0.01f, 1000000.0f, "%.1f m");
    if (clipStartChanged || clipEndChanged) {
        camera.setClipPlanes(clipStart, clipEnd);
    }
    float speed = camera.speed();
    if (ImGui::DragFloat("Fly Speed", &speed, 0.05f, 0.05f, 1000.0f, "%.2f m/s", ImGuiSliderFlags_Logarithmic)) {
        camera.setSpeed(speed);
    }
    ImGui::TextDisabled("Distance to pivot: %.2f m", camera.distance());
}
} // namespace ViewportChromeWidgets
