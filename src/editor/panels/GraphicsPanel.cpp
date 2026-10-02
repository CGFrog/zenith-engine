#include "GraphicsPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/interaction/GraphicsSettingsEditor.hpp"
#include "editor/panels/EditorContext.hpp"

#include <imgui.h>

#include <utility>

GraphicsPanel::GraphicsPanel(EditorRenderSettings settings)
    : m_settings(std::move(settings)) {}

const char* GraphicsPanel::title() const {
    return "Graphics";
}

void GraphicsPanel::loadSettings(RenderRuntime& rendering, EditorDiagnostics& diagnostics) {
    if (const EditorRenderSettingsResult result = m_settings.load(); !result) {
        diagnostics.reportError(result.error);
    }
    apply(rendering);
}

void GraphicsPanel::apply(RenderRuntime& rendering) {
    m_fallbacks = m_settings.apply(rendering);
}

void GraphicsPanel::draw(EditorContext& context) {
    RenderRuntime& rendering = context.app.rendering;
    ImGui::BeginDisabled(!m_settings.unsaved());
    if (ImGui::Button("Save")) {
        if (const EditorRenderSettingsResult result = m_settings.save(); !result) {
            context.diagnostics.reportError(result.error);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        loadSettings(rendering, context.diagnostics);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s %s", m_settings.savesToProject() ? "Project:" : "Editor:",
                        m_settings.storagePath().filename().string().c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", m_settings.storagePath().string().c_str());
    }
    for (const std::string& fallback : m_fallbacks) {
        ImGui::TextWrapped("Fallback: %s", fallback.c_str());
    }

    if (ImGui::CollapsingHeader("GPU pass timings", ImGuiTreeNodeFlags_DefaultOpen)) {
        GraphicsSettingsEditor::drawPassTimings(rendering.stats());
    }
    RenderSettings edited = m_settings.requested();
    if (GraphicsSettingsEditor::drawSettings(edited, rendering.capabilities())) {
        m_settings.request(edited);
        apply(rendering);
    }
}
