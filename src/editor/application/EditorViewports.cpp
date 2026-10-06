// EditorSession's viewports: the per-frame bookkeeping that keeps the active
// viewport, modal transforms, and the renderer's active view consistent. The
// view3d operators acting on the active viewport are the viewport and 3D
// cursor operator families (editorOperatorFamilies).

#include "EditorSession.hpp"

#include <imgui.h>

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

std::vector<ViewportPanel*> EditorSession::levelViewports() const {
    std::vector<ViewportPanel*> viewports = m_viewports.openViewports();
    std::erase_if(viewports, [this](const ViewportPanel* viewport) { return !viewport->showsDocument(m_document); });
    return viewports;
}

ViewportPanel* EditorSession::activeLevelViewport() const {
    if (ViewportPanel* active = m_viewports.active(); active != nullptr && active->showsDocument(m_document)) {
        return active;
    }
    const std::vector<ViewportPanel*> viewports = levelViewports();
    return viewports.empty() ? nullptr : viewports.front();
}

void EditorSession::syncActiveRenderView(ApplicationContext& context) {
    // Shadows and streaming follow a view of the application's scene.
    const ViewportPanel* viewport = activeLevelViewport();
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
