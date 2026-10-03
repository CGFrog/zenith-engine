#include "RenderSettingsControls.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/interaction/GraphicsSettingsEditor.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/settings/RenderSettingsController.hpp"

#include <imgui.h>

#include <string>

namespace RenderSettingsControls {
void drawStorage(EditorContext& context, RenderSettingsController& controller) {
    const EditorRenderSettings& settings = controller.settings();
    ImGui::BeginDisabled(!settings.unsaved());
    if (ImGui::Button("Save")) {
        if (const EditorRenderSettingsResult result = controller.save(); !result) {
            context.diagnostics.reportError(result.error);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        if (const EditorRenderSettingsResult result = controller.load(context.app.rendering); !result) {
            context.diagnostics.reportError(result.error);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s %s", settings.savesToProject() ? "Project:" : "Editor:",
                        settings.storagePath().filename().string().c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", settings.storagePath().string().c_str());
    }
    for (const std::string& fallback : controller.fallbacks()) {
        ImGui::TextWrapped("Fallback: %s", fallback.c_str());
    }
}

bool drawSettings(EditorContext& context, RenderSettingsController& controller) {
    RenderRuntime& rendering = context.app.rendering;
    RenderSettings edited = controller.settings().requested();
    if (!GraphicsSettingsEditor::drawSettings(edited, rendering.capabilities())) {
        return false;
    }
    controller.request(edited, rendering);
    return true;
}
} // namespace RenderSettingsControls
