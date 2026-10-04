// Multiple viewports: open a second Viewport beside the first, give the two
// different cameras and shading, check both render views rendered with their
// own settings while the project's settings stay untouched, then close the
// second and check its view was destroyed and the first became active. The
// smoke may share the user's workspaces.json, so the layout and areas the
// step changed are restored.

#include "editor/areas/AreaWindows.hpp"
#include "editor/areas/Workspace.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

namespace {

class MultiViewportSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "multiple viewports";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        switch (stepFrame) {
        case 0:
            showViewport(context);
            return SmokeStepResult::running();
        case 1:
            // A frame after the area is shown, so its dock node exists.
            return openSecondViewport(context);
        case 2:
            return configureViewports(context);
        case 7:
            // Both views have rendered several frames with their settings.
            return checkViewports(context);
        case 8:
            return checkClosed(context);
        default:
            return SmokeStepResult::running();
        }
    }

  private:
    void showViewport(SmokeStepContext& context) {
        m_viewCount = context.app.rendering.viewCount();
        m_projectShading = context.app.rendering.settings().viewShading;
        if (!context.editor.activeWorkspace().anyVisible("viewport")) {
            m_showedViewport = true;
            context.editor.invokeOperator("wm.toggle_panel", R"({"panel":"viewport"})", KeymapRegion::Global);
        }
    }

    SmokeStepResult openSecondViewport(SmokeStepContext& context) {
        Workspace& workspace = context.editor.activeWorkspace();
        uint32_t firstDock = 0;
        std::vector<std::string> before;
        for (const AreaInstance& area : workspace.areas()) {
            if (area.typeId == "viewport" && area.visible) {
                before.push_back(area.id());
                firstDock = firstDock == 0 ? area.dockId : firstDock;
            }
        }
        m_layout = captureImGuiLayout();
        context.editor.invokeOperator("wm.area_new", R"({"type":"viewport"})", KeymapRegion::Global);
        for (AreaInstance& area : workspace.areas()) {
            if (area.typeId == "viewport" && std::ranges::find(before, area.id()) == before.end()) {
                m_secondArea = area.id();
                // Beside the first viewport rather than tabbed behind it, so
                // both are drawn and render every frame.
                if (firstDock != 0 && ImGui::DockBuilderGetNode(firstDock) != nullptr) {
                    area.dockRequest = ImGui::DockBuilderSplitNode(firstDock, ImGuiDir_Right, 0.5f, nullptr, nullptr);
                } else {
                    area.dockRequest = 0;
                }
            }
        }
        if (m_secondArea.empty()) {
            return SmokeStepResult::failed("wm.area_new did not open a second Viewport");
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult configureViewports(SmokeStepContext& context) {
        EditorSmokeHooks& editor = context.editor;
        ViewportRegistry& viewports = editor.viewports();
        const AreaInstance* second = editor.activeWorkspace().find(m_secondArea);
        const auto* secondPanel = second != nullptr ? dynamic_cast<const ViewportPanel*>(second->panel.get()) : nullptr;
        const std::vector<ViewportPanel*> open = viewports.openViewports();
        const auto first = std::ranges::find_if(open, [&](const ViewportPanel* panel) { return panel != secondPanel; });
        if (secondPanel == nullptr || first == open.end() || !viewports.open(secondPanel->id())) {
            return SmokeStepResult::failed("the two viewports were not both drawn");
        }
        m_firstViewport = (*first)->id();
        m_secondViewport = secondPanel->id();
        m_secondView = secondPanel->renderView();
        if (!m_secondView || m_secondView == (*first)->renderView()) {
            return SmokeStepResult::failed("the second viewport should own a render view of its own");
        }
        // Operators act on the active viewport.
        viewports.activate(m_firstViewport);
        editor.invokeOperator("view3d.shading_set", R"({"type":"rendered"})", KeymapRegion::Viewport);
        editor.invokeOperator("view3d.view_axis", R"({"type":"front"})", KeymapRegion::Viewport);
        // Every scene overlay on, for every object, in the first viewport.
        viewports.activate(m_firstViewport);
        m_overlayState = (*first)->areaState();
        for (const char* overlay :
             {"lights", "cameras", "colliders", "reflection_probes", "decals", "irradiance_volumes", "names"}) {
            const std::string properties = nlohmann::json{{"overlay", overlay}, {"scope", "all"}}.dump();
            if (editor.invokeOperator("view3d.overlay_scope_set", properties, KeymapRegion::Viewport).status !=
                OperatorStatus::Finished) {
                return SmokeStepResult::failed(std::string("view3d.overlay_scope_set failed for ") + overlay);
            }
        }
        viewports.activate(m_secondViewport);
        editor.invokeOperator("view3d.shading_set", R"({"type":"wireframe"})", KeymapRegion::Viewport);
        editor.invokeOperator("view3d.view_axis", R"({"type":"top"})", KeymapRegion::Viewport);
        return SmokeStepResult::running();
    }

    SmokeStepResult checkViewports(SmokeStepContext& context) {
        EditorSmokeHooks& editor = context.editor;
        RenderRuntime& rendering = context.app.rendering;
        ViewportPanel* first = editor.viewports().find(m_firstViewport);
        ViewportPanel* second = editor.viewports().find(m_secondViewport);
        if (first == nullptr || second == nullptr) {
            return SmokeStepResult::failed("a viewport closed during the smoke");
        }
        for (const ViewportPanel* viewport : {first, second}) {
            const RenderViewStats stats = rendering.viewStats(viewport->renderView());
            if (!stats.rendered || stats.submittedFrame == 0 || !rendering.viewTexture(viewport->renderView())) {
                return SmokeStepResult::failed("a viewport's render view did not render");
            }
        }
        if (first->chrome().shading != ViewportShading::Rendered ||
            second->chrome().shading != ViewportShading::Wireframe) {
            return SmokeStepResult::failed("view3d.shading_set did not act on the active viewport");
        }
        const std::optional<RenderSettings>& secondSettings = second->appliedViewSettings();
        const bool secondReduced = secondSettings && (secondSettings->viewShading == ViewShading::Wireframe ||
                                                      secondSettings->viewShading == ViewShading::Solid);
        const std::optional<RenderSettings>& firstSettings = first->appliedViewSettings();
        if ((firstSettings && firstSettings->viewShading != ViewShading::Rendered) || !secondReduced) {
            return SmokeStepResult::failed("each render view should carry its own viewport's shading");
        }
        if (rendering.settings().viewShading != m_projectShading ||
            editor.requestedRenderSettings().viewShading != m_projectShading) {
            return SmokeStepResult::failed("viewport shading leaked into the project's render settings");
        }
        if (glm::length(first->lens(context.app.scene).position - second->lens(context.app.scene).position) < 1e-3f) {
            return SmokeStepResult::failed("the viewports should look from different cameras");
        }
        // With every name shown, the first view projects labels for the
        // scene's objects; the second (names of the selection only) has its
        // own list.
        if (context.app.scene.registry().view<const NameComponent>().size() != 0 &&
            rendering.viewDebugLabels(first->renderView()).empty()) {
            return SmokeStepResult::failed("the scene overlays published no name labels in the first viewport");
        }
        first->restoreAreaState(m_overlayState);
        // Camera view through a scene camera, when the scene has one.
        if (!context.app.scene.registry().view<const CameraComponent>().empty()) {
            editor.viewports().activate(m_secondViewport);
            const OperatorResult cameraView = editor.invokeOperator("view3d.view_camera", {}, KeymapRegion::Viewport);
            if (cameraView.status != OperatorStatus::Finished || !second->cameraView()) {
                return SmokeStepResult::failed("view3d.view_camera did not look through the scene camera");
            }
            second->exitCameraView();
        }
        editor.invokeOperator("wm.area_close", nlohmann::json{{"area", m_secondArea}}.dump(), KeymapRegion::Global);
        return SmokeStepResult::running();
    }

    SmokeStepResult checkClosed(SmokeStepContext& context) {
        EditorSmokeHooks& editor = context.editor;
        RenderRuntime& rendering = context.app.rendering;
        if (editor.viewports().find(m_secondViewport) != nullptr || rendering.viewAlive(m_secondView) ||
            rendering.viewCount() != m_viewCount) {
            return SmokeStepResult::failed("closing a viewport should destroy its render view");
        }
        const ViewportPanel* active = editor.viewports().active();
        if (active == nullptr || active->id() != m_firstViewport) {
            return SmokeStepResult::failed("closing the active viewport should fall back to the open one");
        }
        if (m_showedViewport) {
            editor.invokeOperator("wm.toggle_panel", R"({"panel":"viewport"})", KeymapRegion::Global);
        }
        editor.restoreWorkspaceLayout(m_layout);
        return SmokeStepResult::done();
    }

    // The second Viewport area the step opened, the two viewports it
    // compares, and the second one's render view.
    std::string m_secondArea;
    bool m_showedViewport{false};
    ViewportId m_firstViewport;
    ViewportId m_secondViewport;
    RenderViewId m_secondView;
    uint32_t m_viewCount{0};
    ViewShading m_projectShading{ViewShading::Rendered};
    // The ImGui layout from before the second viewport was docked beside the
    // first, restored at the end.
    std::string m_layout;
    // The first viewport's area state before every scene overlay was set to
    // All, restored afterwards.
    std::string m_overlayState;
};

} // namespace

std::unique_ptr<SmokeStep> makeMultiViewportSmokeStep() {
    return std::make_unique<MultiViewportSmokeStep>();
}
