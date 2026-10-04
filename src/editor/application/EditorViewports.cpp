// EditorSession's viewports: the view3d operators acting on the active
// viewport, the per-frame bookkeeping that keeps the active viewport, modal
// transforms, and the renderer's active view consistent.

#include "EditorSession.hpp"

#include "editor/panels/ViewportOperators.hpp"
#include "editor/viewport/Cursor3D.hpp"

#include <imgui.h>

#include <utility>

void EditorSession::registerViewportOperators() {
    for (Operator& op : viewportOperators(m_viewports)) {
        registerOperator(std::move(op));
    }
    for (Operator& op : cursorOperators(m_viewports, m_document)) {
        registerOperator(std::move(op));
    }
}

void EditorSession::beginViewportFrame(ApplicationContext& context) {
    m_viewports.beginFrame();
    m_modalTransformFinishedThisFrame = false;
    // The viewport hide is an authoring aid: Play mode shows the scene as the
    // game would.
    context.rendering.setSceneVisibilityMode(m_playMode.mode() == EditorMode::Edit ? SceneVisibilityMode::Editor
                                                                                   : SceneVisibilityMode::Runtime);
    // The default view renders every frame, so a viewport nobody sees (its
    // area hidden, tabbed away, or in another workspace) gives it up for the
    // next drawn viewport.
    if (ViewportPanel* holder = m_viewports.defaultViewHolder(); holder != nullptr && !m_viewports.open(holder->id())) {
        holder->releaseRenderView();
    }
    // A transform whose viewport closed (or left the workspace) cannot be
    // finished with the mouse, so it is cancelled.
    if (m_activeTransform && !m_viewports.open(m_activeTransform->viewport)) {
        finishModalTransform(context, false);
    }
}

void EditorSession::syncActiveRenderView(ApplicationContext& context) {
    const ViewportPanel* viewport = m_viewports.active();
    if (viewport != nullptr && viewport->renderView()) {
        context.rendering.setActiveView(viewport->renderView());
    }
}

glm::vec2 EditorSession::lastOperatorAnchor() const {
    if (const ViewportPanel* viewport = m_viewports.active()) {
        return viewport->position() + glm::vec2(0.0f, viewport->size().y);
    }
    // Above the status bar at the bottom-left of the work area.
    const ImGuiViewport* main = ImGui::GetMainViewport();
    return {main->WorkPos.x, main->WorkPos.y + main->WorkSize.y - ImGui::GetFrameHeight()};
}
