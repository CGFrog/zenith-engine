// EditorSession's viewports: the view3d operators acting on the active
// viewport, the per-frame bookkeeping that keeps the active viewport, modal
// transforms, and the renderer's active view consistent, and the
// multi-viewport smoke.

#include "EditorSession.hpp"

#include "editor/areas/AreaWindows.hpp"
#include "editor/panels/ViewportOperators.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <limits>

void EditorSession::registerViewportOperators() {
    for (Operator& op : viewportOperators(m_viewports)) {
        registerOperator(std::move(op));
    }
}

void EditorSession::beginViewportFrame(ApplicationContext& context) {
    m_viewports.beginFrame();
    m_modalTransformFinishedThisFrame = false;
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

bool EditorSession::updateSmokeViewports(ApplicationContext& context) {
    // Frames 10-18: open a second Viewport beside the first, give the two
    // different cameras and shading, check both render views rendered with
    // their own settings while the project's settings stay untouched, then
    // close the second and check its view was destroyed and the first became
    // active. The smoke shares the user's workspaces.json, so the layout and
    // areas it changed are restored.
    constexpr uint32_t firstFrame = 10;
    const auto fail = [&](const std::string& message) {
        const std::string error = "Viewport smoke failed: " + message;
        std::cerr << error << '\n';
        m_diagnostics.reportError(error);
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    };
    const auto viewportOf = [this](ViewportId id) { return m_viewports.find(id); };
    RenderRuntime& rendering = context.rendering;
    Workspace& workspace = m_workspaces.active();
    if (m_smokeFrame == firstFrame) {
        m_smokeViewCount = rendering.viewCount();
        m_smokeProjectShading = rendering.settings().viewShading;
        if (!workspace.anyVisible("viewport")) {
            m_smokeShowedViewport = true;
            invokeOperator(context, "wm.toggle_panel", R"({"panel":"viewport"})", KeymapRegion::Global);
        }
    } else if (m_smokeFrame == firstFrame + 1) {
        // A frame after the workspace smoke switched back, so the restored
        // layout's dock nodes exist again.
        uint32_t firstDock = 0;
        std::vector<std::string> before;
        for (const AreaInstance& area : workspace.areas()) {
            if (area.typeId == "viewport" && area.visible) {
                before.push_back(area.id());
                firstDock = firstDock == 0 ? area.dockId : firstDock;
            }
        }
        m_smokeLayout = captureImGuiLayout();
        invokeOperator(context, "wm.area_new", R"({"type":"viewport"})", KeymapRegion::Global);
        for (AreaInstance& area : workspace.areas()) {
            if (area.typeId == "viewport" && std::ranges::find(before, area.id()) == before.end()) {
                m_smokeViewportArea = area.id();
                // Beside the first viewport rather than tabbed behind it, so
                // both are drawn and render every frame.
                if (firstDock != 0 && ImGui::DockBuilderGetNode(firstDock) != nullptr) {
                    area.dockRequest = ImGui::DockBuilderSplitNode(firstDock, ImGuiDir_Right, 0.5f, nullptr, nullptr);
                } else {
                    area.dockRequest = 0;
                }
            }
        }
        if (m_smokeViewportArea.empty()) {
            fail("wm.area_new did not open a second Viewport");
        }
    } else if (m_smokeFrame == firstFrame + 2) {
        const AreaInstance* second = workspace.find(m_smokeViewportArea);
        const auto* secondPanel = second != nullptr ? dynamic_cast<const ViewportPanel*>(second->panel.get()) : nullptr;
        const std::vector<ViewportPanel*> open = m_viewports.openViewports();
        const auto first = std::ranges::find_if(open, [&](const ViewportPanel* panel) { return panel != secondPanel; });
        if (secondPanel == nullptr || first == open.end() || !m_viewports.open(secondPanel->id())) {
            fail("the two viewports were not both drawn");
            return true;
        }
        m_smokeFirstViewport = (*first)->id();
        m_smokeSecondViewport = secondPanel->id();
        m_smokeSecondView = secondPanel->renderView();
        if (!m_smokeSecondView || m_smokeSecondView == (*first)->renderView()) {
            fail("the second viewport should own a render view of its own");
        }
        // Operators act on the active viewport.
        m_viewports.activate(m_smokeFirstViewport);
        invokeOperator(context, "view3d.shading_set", R"({"type":"rendered"})", KeymapRegion::Viewport);
        invokeOperator(context, "view3d.view_axis", R"({"type":"front"})", KeymapRegion::Viewport);
        m_viewports.activate(m_smokeSecondViewport);
        invokeOperator(context, "view3d.shading_set", R"({"type":"wireframe"})", KeymapRegion::Viewport);
        invokeOperator(context, "view3d.view_axis", R"({"type":"top"})", KeymapRegion::Viewport);
    } else if (m_smokeFrame == firstFrame + 7) {
        ViewportPanel* first = viewportOf(m_smokeFirstViewport);
        ViewportPanel* second = viewportOf(m_smokeSecondViewport);
        if (first == nullptr || second == nullptr) {
            fail("a viewport closed during the smoke");
            return true;
        }
        for (const ViewportPanel* viewport : {first, second}) {
            const RenderViewStats stats = rendering.viewStats(viewport->renderView());
            if (!stats.rendered || stats.submittedFrame == 0 || !rendering.viewTexture(viewport->renderView())) {
                fail("a viewport's render view did not render");
            }
        }
        if (first->chrome().shading != ViewportShading::Rendered ||
            second->chrome().shading != ViewportShading::Wireframe) {
            fail("view3d.shading_set did not act on the active viewport");
        }
        const std::optional<RenderSettings>& secondSettings = second->appliedViewSettings();
        const bool secondReduced = secondSettings && (secondSettings->viewShading == ViewShading::Wireframe ||
                                                      secondSettings->viewShading == ViewShading::Solid);
        const std::optional<RenderSettings>& firstSettings = first->appliedViewSettings();
        if ((firstSettings && firstSettings->viewShading != ViewShading::Rendered) || !secondReduced) {
            fail("each render view should carry its own viewport's shading");
        }
        if (rendering.settings().viewShading != m_smokeProjectShading ||
            m_renderSettings.settings().requested().viewShading != m_smokeProjectShading) {
            fail("viewport shading leaked into the project's render settings");
        }
        if (glm::length(first->lens(context.scene).position - second->lens(context.scene).position) < 1e-3f) {
            fail("the viewports should look from different cameras");
        }
        // Camera view through a scene camera, when the scene has one.
        if (!context.scene.registry().view<const CameraComponent>().empty()) {
            m_viewports.activate(m_smokeSecondViewport);
            const OperatorResult cameraView = invokeOperator(context, "view3d.view_camera", {}, KeymapRegion::Viewport);
            if (cameraView.status != OperatorStatus::Finished || !second->cameraView()) {
                fail("view3d.view_camera did not look through the scene camera");
            }
            second->exitCameraView();
        }
        invokeOperator(context, "wm.area_close", nlohmann::json{{"area", m_smokeViewportArea}}.dump(),
                       KeymapRegion::Global);
    } else if (m_smokeFrame == firstFrame + 8) {
        if (viewportOf(m_smokeSecondViewport) != nullptr || rendering.viewAlive(m_smokeSecondView) ||
            rendering.viewCount() != m_smokeViewCount) {
            fail("closing a viewport should destroy its render view");
        }
        const ViewportPanel* active = m_viewports.active();
        if (active == nullptr || active->id() != m_smokeFirstViewport) {
            fail("closing the active viewport should fall back to the open one");
        }
        if (m_smokeShowedViewport) {
            invokeOperator(context, "wm.toggle_panel", R"({"panel":"viewport"})", KeymapRegion::Global);
        }
        restoreImGuiLayout(m_smokeLayout);
        workspace.setLayout(m_smokeLayout);
        m_workspacesDirty = true;
    }
    return m_smokeFrame > firstFrame + 8;
}
