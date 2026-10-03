// Several viewports at once without a window or GPU: render view ownership,
// the active viewport, view3d operators acting on it, per-viewport shading
// that never reaches the project's render settings, and camera view.

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/ViewportOperators.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "engine/application/Application.hpp"
#include "engine/diagnostics/Logger.hpp"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

class OperatorActions final : public EditorSessionActions {
  public:
    OperatorActions(SceneEditing editing, CommandHistory& history, Selection& selection)
        : runner(registry, history)
        , m_editing(editing)
        , m_selection(selection) {}

    OperatorRegistry registry;
    OperatorRunner runner;

    OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override {
        OperatorContext operatorContext{
            .editing = m_editing, .selection = m_selection, .region = region, .app = &context};
        return runner.invokeWithJson(operatorId, operatorContext, properties);
    }

    bool operatorAvailable(ApplicationContext&, std::string_view operatorId) override {
        return registry.find(operatorId) != nullptr;
    }

    std::string operatorShortcut(std::string_view, std::string_view) const override {
        return {};
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path) override {}

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove>) override {}

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path>) override {}

    void handleObjectKeys(ApplicationContext&, KeymapRegion) override {}

    bool modalTransformActive() const override {
        return false;
    }

    void updateModalTransform(ApplicationContext&, ViewportId) override {}

  private:
    SceneEditing m_editing;
    Selection& m_selection;
};

// Everything EditorContext references, plus the session's viewport registry
// with its operators. Viewports are destroyed before the registry and the
// render runtime they hold views of.
struct Fixture {
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    Logger logger;
    ApplicationContext app{scene, assets, physics, audio, control, rendering, gameUi, logger};
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorActions actions{editing(), history, selection};
    EditorMenus menus;
    EditorMode mode{EditorMode::Edit};
    ViewportRegistry viewports;
    std::vector<std::unique_ptr<ViewportPanel>> panels;

    Fixture() {
        for (Operator& op : viewportOperators(viewports)) {
            expect(static_cast<bool>(actions.registry.add(std::move(op))), "the viewport operators should register");
        }
    }

    ~Fixture() {
        panels.clear();
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    SceneEditing editing() {
        return SceneEditing(scene, assets, document, history, diagnostics);
    }

    EditorContext context() {
        return {.app = app,
                .editing = editing(),
                .selection = selection,
                .diagnostics = diagnostics,
                .mode = mode,
                .actions = actions,
                .menus = menus};
    }

    ViewportPanel& addViewport() {
        panels.push_back(std::make_unique<ViewportPanel>(viewports));
        return *panels.back();
    }

    void close(const ViewportPanel& panel) {
        std::erase_if(panels,
                      [&](const std::unique_ptr<ViewportPanel>& candidate) { return candidate.get() == &panel; });
    }

    OperatorResult invoke(std::string_view operatorId, std::string_view properties = {}) {
        return actions.invokeOperator(app, operatorId, properties, KeymapRegion::Viewport);
    }
};

// A headless ImGui context drawing every viewport of a fixture side by side,
// as a session frame does: the registry's frame starts, then each viewport
// updates and draws in its own window.
class SideBySide {
  public:
    SideBySide() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~SideBySide() {
        ImGui::DestroyContext();
    }

    SideBySide(const SideBySide&) = delete;
    SideBySide& operator=(const SideBySide&) = delete;

    // Draws only the given viewports (others count as hidden); the mouse is
    // at mouse.
    void frame(Fixture& fixture, ImVec2 mouse, const std::vector<ViewportPanel*>& drawn) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        fixture.viewports.beginFrame();
        ImGui::NewFrame();
        const float width = io.DisplaySize.x / static_cast<float>(std::max<size_t>(drawn.size(), 1));
        for (size_t index = 0; index < drawn.size(); ++index) {
            ViewportPanel& panel = *drawn[index];
            ImGui::SetNextWindowPos({width * static_cast<float>(index), 0.0f});
            ImGui::SetNextWindowSize({width, io.DisplaySize.y});
            const std::string name = "Viewport##viewport." + std::to_string(panel.id().value);
            ImGui::Begin(name.c_str(), nullptr, panel.windowFlags());
            EditorContext context = fixture.context();
            panel.update(context, InputState{}, io.DeltaTime);
            panel.draw(context);
            ImGui::End();
        }
        ImGui::Render();
    }

    void settle(Fixture& fixture, ImVec2 mouse, const std::vector<ViewportPanel*>& drawn) {
        for (int index = 0; index < 3; ++index) {
            frame(fixture, mouse, drawn);
        }
    }
};

ImVec2 centerOf(const ViewportPanel& panel) {
    return {panel.position().x + panel.size().x * 0.5f, panel.position().y + panel.size().y * 0.5f};
}

void testRenderViewLifetime() {
    Fixture fixture;
    SideBySide imgui;
    ViewportPanel& first = fixture.addViewport();
    ViewportPanel& second = fixture.addViewport();
    ViewportPanel& third = fixture.addViewport();
    expect(!first.renderView() && !second.renderView(), "a viewport should take a render view only once drawn");
    imgui.settle(fixture, {5.0f, 5.0f}, {&first, &second, &third});
    expect(first.renderView() == fixture.rendering.defaultView(),
           "the first viewport drawn should use the renderer's default view");
    expect(second.renderView() && third.renderView() && second.renderView() != third.renderView() &&
               second.renderView() != fixture.rendering.defaultView(),
           "further viewports should own views of their own");
    expect(fixture.rendering.viewCount() == 3, "three viewports should hold three views");

    const RenderViewId secondView = second.renderView();
    fixture.close(second);
    expect(!fixture.rendering.viewAlive(secondView) && fixture.rendering.viewCount() == 2,
           "closing a viewport should destroy its render view");

    const RenderViewId defaultView = fixture.rendering.defaultView();
    fixture.close(first);
    expect(fixture.rendering.viewAlive(defaultView) && fixture.rendering.viewCount() == 2,
           "closing the default view's viewport should keep the default view but release it");
    ViewportPanel& fourth = fixture.addViewport();
    imgui.settle(fixture, {5.0f, 5.0f}, {&third, &fourth});
    expect(third.renderView() == defaultView && fourth.renderView() && fourth.renderView() != defaultView,
           "the next drawn viewport should trade its own view for a released default view");
    expect(fixture.rendering.viewCount() == 2, "trading views should not leak the traded view");
    expect(fixture.viewports.size() == 2, "the registry should only list live viewports");

    fixture.panels.clear();
    expect(fixture.rendering.viewCount() == 1 && fixture.viewports.size() == 0,
           "closing every viewport should leave only the default view");
}

void testActiveViewportFollowsTheMouse() {
    Fixture fixture;
    SideBySide imgui;
    ViewportPanel& left = fixture.addViewport();
    ViewportPanel& right = fixture.addViewport();
    const std::vector<ViewportPanel*> both{&left, &right};
    imgui.settle(fixture, {5.0f, 700.0f}, both);

    imgui.settle(fixture, centerOf(right), both);
    expect(fixture.viewports.active() == &right, "hovering a viewport should make it active");
    expect(static_cast<bool>(fixture.invoke("view3d.toggle_sidebar")) && right.chrome().sidebarVisible &&
               !left.chrome().sidebarVisible,
           "view3d.toggle_sidebar should act on the active viewport only");
    expect(static_cast<bool>(fixture.invoke("view3d.shading_set", R"({"type":"wireframe"})")) &&
               right.chrome().shading == ViewportShading::Wireframe &&
               left.chrome().shading == ViewportShading::Rendered,
           "view3d.shading_set should act on the active viewport only");
    expect(static_cast<bool>(fixture.invoke("view3d.toggle_snapping")) && right.tools().snapping &&
               !left.tools().snapping,
           "view3d.toggle_snapping should act on the active viewport only");
    expect(static_cast<bool>(fixture.invoke("view3d.view_axis", R"({"type":"top"})")),
           "view3d.view_axis should run on the active viewport");

    imgui.settle(fixture, centerOf(left), both);
    expect(fixture.viewports.active() == &left, "moving to another viewport should activate it");
    expect(static_cast<bool>(fixture.invoke("view3d.view_axis", R"({"type":"front"})")),
           "view3d.view_axis should run on the newly active viewport");
    expect(glm::length(left.camera().renderView().forward - right.camera().renderView().forward) > 0.5f,
           "each viewport should keep its own camera");
}

void testClosingTheActiveViewport() {
    Fixture fixture;
    SideBySide imgui;
    ViewportPanel& left = fixture.addViewport();
    ViewportPanel& right = fixture.addViewport();
    imgui.settle(fixture, {960.0f, 360.0f}, {&left, &right});
    expect(fixture.viewports.active() == &right, "the right viewport should be active");

    fixture.close(right);
    expect(fixture.viewports.active() == &left, "closing the active viewport should fall back to an open one");
    expect(static_cast<bool>(fixture.invoke("view3d.toggle_toolbar")) && !left.chrome().toolbarVisible,
           "operators should act on the fallback viewport");

    // A viewport that is no longer drawn (its area hidden or in another
    // workspace) is not open, so nothing is active.
    imgui.frame(fixture, {5.0f, 5.0f}, {});
    imgui.frame(fixture, {5.0f, 5.0f}, {});
    expect(fixture.viewports.active() == nullptr, "a viewport that is not drawn should not stay active");
    const OperatorResult result = fixture.invoke("view3d.toggle_sidebar");
    expect(result.status == OperatorStatus::Cancelled && !left.chrome().sidebarVisible,
           "view3d operators should be unavailable without an open viewport");
    for (const char* operatorId : {"view3d.view_axis", "view3d.frame_all", "view3d.view_camera"}) {
        expect(fixture.invoke(operatorId).status == OperatorStatus::Cancelled,
               "every view3d operator should be unavailable without an open viewport");
    }

    fixture.close(left);
    expect(fixture.viewports.active() == nullptr && fixture.viewports.size() == 0,
           "closing every viewport should leave none active");
}

void testShadingStaysOutOfProjectSettings() {
    RenderSettings project;
    project.ambientOcclusion = AmbientOcclusionQuality::Off;
    RenderCapabilities capabilities;
    capabilities.wireframe = true;
    expect(!viewportViewSettings(project, capabilities, ViewportShading::Rendered, EditorMode::Edit),
           "a Rendered viewport should follow the project's settings");
    const std::optional<RenderSettings> wireframe =
        viewportViewSettings(project, capabilities, ViewportShading::Wireframe, EditorMode::Edit);
    expect(wireframe && wireframe->viewShading == ViewShading::Wireframe,
           "a Wireframe viewport should override its view's shading");
    expect(project.viewShading == ViewShading::Rendered, "the project's settings should be left unchanged");
    expect(!viewportViewSettings(project, capabilities, ViewportShading::Wireframe, EditorMode::Play),
           "Play mode should show the project's own look");
    capabilities.wireframe = false;
    const std::optional<RenderSettings> fallback =
        viewportViewSettings(project, capabilities, ViewportShading::Wireframe, EditorMode::Edit);
    expect(fallback && fallback->viewShading == ViewShading::Solid,
           "Wireframe should fall back to Solid without the wireframe capability");

    Fixture fixture;
    SideBySide imgui;
    ViewportPanel& left = fixture.addViewport();
    ViewportPanel& right = fixture.addViewport();
    right.chrome().shading = ViewportShading::Solid;
    imgui.settle(fixture, {5.0f, 5.0f}, {&left, &right});
    expect(!left.appliedViewSettings() && right.appliedViewSettings() &&
               right.appliedViewSettings()->viewShading == ViewShading::Solid,
           "each viewport should hand its own shading to its own view");
    expect(fixture.rendering.settings().viewShading == ViewShading::Rendered,
           "viewport shading should never be written into the renderer's project settings");
    fixture.mode = EditorMode::Play;
    imgui.frame(fixture, {5.0f, 5.0f}, {&left, &right});
    expect(!right.appliedViewSettings(), "Play mode should not inherit the viewport's shading");
}

void testCameraView() {
    Fixture fixture;
    SideBySide imgui;
    ViewportPanel& viewport = fixture.addViewport();
    imgui.settle(fixture, {640.0f, 360.0f}, {&viewport});

    expect(fixture.invoke("view3d.view_camera").status == OperatorStatus::Failed,
           "camera view should fail descriptively without a scene camera");

    const Entity cameraEntity = fixture.scene.createEntity("Shot Camera");
    fixture.scene.registry().emplace<CameraComponent>(
        cameraEntity, CameraComponent{.verticalFovRadians = 0.5f, .nearPlane = 0.2f, .farPlane = 300.0f});
    fixture.scene.registry().get<TransformComponent>(cameraEntity).local =
        glm::translate(glm::mat4(1.0f), {3.0f, 4.0f, 5.0f});
    fixture.scene.markTransformDirty(cameraEntity);
    fixture.scene.updateWorldTransforms();
    const Entity plain = fixture.scene.createEntity("Not a camera");
    expect(!viewport.lookThroughCamera(fixture.scene, fixture.scene.id(plain)),
           "only a camera object can be looked through");

    expect(static_cast<bool>(fixture.invoke("view3d.view_camera")) &&
               viewport.cameraView() == fixture.scene.id(cameraEntity),
           "view3d.view_camera should look through the scene camera");
    const RenderView lens = viewport.lens(fixture.scene);
    expect(glm::length(lens.position - glm::vec3(3.0f, 4.0f, 5.0f)) < 1e-4f && lens.verticalFovRadians == 0.5f &&
               lens.nearPlane == 0.2f && lens.farPlane == 300.0f,
           "the viewport lens should be the scene camera's");

    expect(static_cast<bool>(fixture.invoke("view3d.view_camera")) && !viewport.cameraView(),
           "view3d.view_camera should toggle the camera view off");
    expect(viewport.lookThroughCamera(fixture.scene, fixture.scene.id(cameraEntity)),
           "the View menu should look through a chosen camera");
    expect(static_cast<bool>(fixture.invoke("view3d.view_orbit", R"({"direction":"left"})")) && !viewport.cameraView(),
           "navigating should leave the camera view, as in Blender");

    viewport.lookThroughCamera(fixture.scene, fixture.scene.id(cameraEntity));
    fixture.scene.destroyEntity(cameraEntity);
    imgui.frame(fixture, {640.0f, 360.0f}, {&viewport});
    expect(!viewport.cameraView(), "deleting the camera should end the camera view");
}
} // namespace

int main() {
    testRenderViewLifetime();
    testActiveViewportFollowsTheMouse();
    testClosingTheActiveViewport();
    testShadingStaysOutOfProjectSettings();
    testCameraView();
    if (failures != 0) {
        std::cerr << failures << " multi-viewport test(s) failed\n";
        return 1;
    }
    return 0;
}
