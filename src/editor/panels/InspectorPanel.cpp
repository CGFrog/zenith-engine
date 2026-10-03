#include "InspectorPanel.hpp"

#include "editor/panels/EditorContext.hpp"

#include <imgui.h>

InspectorPanel::InspectorPanel(ComponentInspectorLayout& layout)
    : m_view(layout) {}

const char* InspectorPanel::title() const {
    return "Inspector";
}

bool InspectorPanel::editModeOnly() const {
    return true;
}

void InspectorPanel::draw(EditorContext& context) {
    context.selection.removeMissing(context.app.scene);
    if (context.selection.empty()) {
        ImGui::TextDisabled("Select an entity to inspect it.");
        return;
    }
    if (context.selection.size() != 1) {
        m_view.drawMultipleSelection(context);
        ImGui::Separator();
        m_view.drawFilter("##InspectorFilter", m_filter);
        m_view.drawComponents(context, context.selection.ids(), m_filter);
        return;
    }
    const SceneEntityId entityId = *context.selection.active();
    if (context.app.scene.findEntity(entityId) == entt::null) {
        return;
    }
    m_view.drawName(context, entityId);
    ImGui::Separator();
    m_view.drawFilter("##InspectorFilter", m_filter);
    m_view.drawTransform(context, entityId, m_filter);
    m_view.drawComponents(context, context.selection.ids(), m_filter);
    m_view.drawPreservedComponents(context, entityId);
    m_view.drawIrradianceBakeButton(context, entityId);
    m_view.drawAssetWarnings(context, entityId);
    m_view.drawModelDropTarget(context, entityId);
    ImGui::Separator();
    m_view.drawAddComponentMenu(context, entityId);
}
