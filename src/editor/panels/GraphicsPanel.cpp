#include "GraphicsPanel.hpp"

#include "editor/interaction/GraphicsSettingsEditor.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/RenderSettingsControls.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

GraphicsPanel::GraphicsPanel(RenderSettingsController& settings)
    : m_settings(settings) {}

const char* GraphicsPanel::title() const {
    return "Graphics";
}

void GraphicsPanel::draw(EditorContext& context) {
    RenderSettingsControls::drawStorage(context, m_settings);
    if (widgets::SectionHeader("GPU pass timings", ImGuiTreeNodeFlags_DefaultOpen)) {
        GraphicsSettingsEditor::drawPassTimings(context.app.rendering.stats());
    }
    RenderSettingsControls::drawSettings(context, m_settings);
}
