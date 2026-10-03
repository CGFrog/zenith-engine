#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "engine/application/Application.hpp"
#include "engine/diagnostics/Logger.hpp"

#include <imgui.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// The session's operator and keymap services without a window: operators run
// through a real registry, runner, and default keymap, and region keys are
// dispatched the way the session does.
class HeadlessSessionActions final : public EditorSessionActions {
  public:
    HeadlessSessionActions(SceneEditing editing, CommandHistory& history, Selection& selection)
        : runner(registry, history)
        , m_editing(editing)
        , m_selection(selection) {}

    OperatorRegistry registry;
    OperatorRunner runner;
    Keymap keymap{defaultEditorKeymap()};

    OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override {
        OperatorContext operatorContext{
            .editing = m_editing, .selection = m_selection, .region = region, .app = &context};
        return runner.invokeWithJson(operatorId, operatorContext, properties);
    }

    bool operatorAvailable(ApplicationContext&, std::string_view operatorId) override {
        return registry.find(operatorId) != nullptr;
    }

    std::string operatorShortcut(std::string_view operatorId, std::string_view properties) const override {
        return keymap.shortcutLabel(operatorId, properties);
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path) override {}

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove>) override {}

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path>) override {}

    void handleObjectKeys(ApplicationContext& context, KeymapRegion region) override {
        for (const ImGuiKeyChord chord : keymap.chords(region)) {
            if (!ImGui::IsKeyChordPressed(chord)) {
                continue;
            }
            const KeymapBinding* binding = keymap.resolve(region, chord, ImGui::GetTime(), 0.3);
            if (binding != nullptr) {
                const std::string operatorId = binding->operatorId;
                const std::string properties = binding->properties;
                invokeOperator(context, operatorId, properties, region);
            }
            return;
        }
    }

    bool modalTransformActive() const override {
        return false;
    }

    void updateModalTransform(ApplicationContext&) override {}

  private:
    SceneEditing m_editing;
    Selection& m_selection;
};

// Everything EditorContext references, owned together.
struct ViewportFixture {
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
    EditorCamera camera;
    HeadlessSessionActions actions{editing(), history, selection};
    ViewportPanel viewport;

    ViewportFixture() {
        for (Operator& op : viewport.chromeOperators()) {
            expect(static_cast<bool>(actions.registry.add(std::move(op))), "the chrome operators should register");
        }
    }

    SceneEditing editing() {
        return SceneEditing(scene, assets, document, history, diagnostics);
    }

    EditorContext context() {
        return {.app = app,
                .editing = editing(),
                .selection = selection,
                .diagnostics = diagnostics,
                .camera = camera,
                .mode = EditorMode::Edit,
                .actions = actions};
    }

    OperatorResult invoke(std::string_view operatorId, std::string_view properties = {}) {
        return actions.invokeOperator(app, operatorId, properties, KeymapRegion::Global);
    }
};

// Drives a headless ImGui context: no renderer, just enough state for the
// viewport window and its chrome child windows to lay out and take input.
class HeadlessImGui {
  public:
    HeadlessImGui() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.IniFilename = nullptr;
        // Every queued event applies in the frame it was queued for.
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~HeadlessImGui() {
        ImGui::DestroyContext();
    }

    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    // One frame with the mouse at mouse; a click presses the left button
    // this frame and releases it the next, and a key is tapped the same way.
    void frame(ViewportFixture& fixture, ImVec2 mouse, bool click = false, ImGuiKey key = ImGuiKey_None) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, click);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, true);
        }
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin(fixture.viewport.title(), nullptr, fixture.viewport.windowFlags());
        EditorContext context = fixture.context();
        fixture.viewport.update(context, InputState{}, io.DeltaTime);
        fixture.viewport.draw(context);
        ImGui::End();
        ImGui::Render();
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, false);
        }
        if (click) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        }
    }

    // A few idle frames, so windows exist and hover state is settled.
    void settle(ViewportFixture& fixture, ImVec2 mouse) {
        for (int index = 0; index < 3; ++index) {
            frame(fixture, mouse);
        }
    }
};

ImVec2 imageCenter(const ViewportPanel& viewport) {
    return {viewport.position().x + viewport.size().x * 0.5f, viewport.position().y + viewport.size().y * 0.5f};
}

// A point inside the tool shelf, which sits below the header on the left.
ImVec2 toolShelfPoint(const ViewportPanel& viewport) {
    return {viewport.position().x + 12.0f, viewport.position().y + ImGui::GetFrameHeight() * 2.0f};
}

// A point inside the sidebar, which runs down the right edge.
ImVec2 sidebarPoint(const ViewportPanel& viewport) {
    return {viewport.position().x + viewport.size().x - 20.0f, viewport.position().y + viewport.size().y * 0.75f};
}

SceneEntityId selectNewEntity(ViewportFixture& fixture) {
    const SceneEntityId id = fixture.scene.id(fixture.scene.createEntity("Chrome target"));
    fixture.selection.selectOnly(id);
    return id;
}

void testDefaultsAndPickingIgnoresChrome() {
    ViewportFixture fixture;
    HeadlessImGui imgui;
    expect(fixture.viewport.chrome().toolbarVisible && !fixture.viewport.chrome().sidebarVisible,
           "the viewport should start like Blender: tool shelf shown, sidebar hidden");
    expect(fixture.viewport.chrome().shading == ViewportShading::Rendered, "the viewport should start rendered");
    imgui.settle(fixture, {640.0f, 360.0f});
    expect(fixture.viewport.size().x > 100.0f && fixture.viewport.size().y > 100.0f,
           "the image area should be laid out without a rendered texture");

    const SceneEntityId selected = selectNewEntity(fixture);
    const ImVec2 shelf = toolShelfPoint(fixture.viewport);
    imgui.settle(fixture, shelf);
    imgui.frame(fixture, shelf, true);
    imgui.frame(fixture, shelf);
    expect(fixture.selection.active() == selected, "a click on the tool shelf should not pick through it");

    const ImVec2 center = imageCenter(fixture.viewport);
    imgui.settle(fixture, center);
    imgui.frame(fixture, center, true);
    imgui.frame(fixture, center);
    expect(fixture.selection.empty(), "a click on empty image space should pick nothing and clear the selection");
}

void testKeysToggleRegions() {
    ViewportFixture fixture;
    HeadlessImGui imgui;
    imgui.settle(fixture, {640.0f, 360.0f});
    const ImVec2 hovered = imageCenter(fixture.viewport);
    imgui.settle(fixture, hovered);

    imgui.frame(fixture, hovered, false, ImGuiKey_N);
    imgui.frame(fixture, hovered);
    expect(fixture.viewport.chrome().sidebarVisible, "N over the viewport should open the sidebar");

    // The sidebar now covers part of the image; clicks there must not pick,
    // and its Item tab draws the active object's transform.
    const SceneEntityId selected = selectNewEntity(fixture);
    const ImVec2 sidebar = sidebarPoint(fixture.viewport);
    imgui.settle(fixture, sidebar);
    imgui.frame(fixture, sidebar, true);
    imgui.frame(fixture, sidebar);
    expect(fixture.selection.active() == selected, "a click on the sidebar should not pick through it");

    // N works while the mouse is over the sidebar too, as in Blender.
    imgui.frame(fixture, sidebar, false, ImGuiKey_N);
    imgui.frame(fixture, sidebar);
    expect(!fixture.viewport.chrome().sidebarVisible, "N over the sidebar should close it");

    imgui.settle(fixture, hovered);
    imgui.frame(fixture, hovered, false, ImGuiKey_T);
    imgui.frame(fixture, hovered);
    expect(!fixture.viewport.chrome().toolbarVisible, "T over the viewport should hide the tool shelf");

    // With the shelf hidden, its old place is image again and picks.
    const ImVec2 shelf = toolShelfPoint(fixture.viewport);
    imgui.settle(fixture, shelf);
    imgui.frame(fixture, shelf, true);
    imgui.frame(fixture, shelf);
    expect(fixture.selection.empty(), "a click where the hidden tool shelf was should pick the image");

    expect(fixture.actions.operatorShortcut("view3d.toggle_sidebar", {}) == "N" &&
               fixture.actions.operatorShortcut("view3d.toggle_toolbar", {}) == "T",
           "the header's View menu should show the N and T shortcuts");
}

void testOperators() {
    ViewportFixture fixture;
    expect(static_cast<bool>(fixture.invoke("view3d.toggle_sidebar")) && fixture.viewport.chrome().sidebarVisible,
           "view3d.toggle_sidebar should show the sidebar");
    expect(static_cast<bool>(fixture.invoke("view3d.toggle_toolbar")) && !fixture.viewport.chrome().toolbarVisible,
           "view3d.toggle_toolbar should hide the tool shelf");
    expect(static_cast<bool>(fixture.invoke("view3d.shading_set", R"({"type":"wireframe"})")) &&
               fixture.viewport.chrome().shading == ViewportShading::Wireframe,
           "view3d.shading_set should store the panel's shading");
    expect(static_cast<bool>(fixture.invoke("view3d.shading_set", R"({"type":"material_preview"})")) &&
               fixture.viewport.chrome().shading == ViewportShading::MaterialPreview,
           "view3d.shading_set should accept every shading mode");
    expect(fixture.invoke("view3d.shading_set", R"({"type":"x-ray"})").status == OperatorStatus::Failed,
           "an unknown shading mode should fail");
    expect(!fixture.history.canUndo(), "chrome operators should not record undo steps");
}

void testSidebarTabsAndChromeDrawing() {
    ViewportFixture fixture;
    HeadlessImGui imgui;
    ViewportPanel& viewport = fixture.viewport;
    const auto drawTab = [](EditorContext&) { ImGui::TextUnformatted("Module tab"); };
    expect(static_cast<bool>(viewport.addSidebarTab({.title = "Spinners", .draw = drawTab})),
           "a module tab should be added");
    expect(!viewport.addSidebarTab({.title = "Spinners", .draw = drawTab}), "duplicate tab titles should be refused");
    expect(!viewport.addSidebarTab({.title = "View", .draw = drawTab}), "built-in tab titles should be refused");
    expect(!viewport.addSidebarTab({.title = "", .draw = drawTab}), "tabs need a title");
    expect(!viewport.addSidebarTab({.title = "Empty"}), "tabs need a draw function");

    // Every region and overlay combination draws headless.
    fixture.invoke("view3d.toggle_sidebar");
    selectNewEntity(fixture);
    viewport.tools().gizmo = GizmoMode::Select;
    for (const ViewportShading shading : {ViewportShading::Rendered, ViewportShading::MaterialPreview,
                                          ViewportShading::Solid, ViewportShading::Wireframe}) {
        viewport.chrome().shading = shading;
        viewport.chrome().overlays = {.grid = shading != ViewportShading::Solid,
                                      .selectionOutline = shading != ViewportShading::Wireframe,
                                      .activeName = shading == ViewportShading::Rendered,
                                      .gizmo = false};
        imgui.settle(fixture, {10.0f, 10.0f});
    }
    expect(viewport.removeSidebarTab("Spinners") && !viewport.removeSidebarTab("Spinners"),
           "a tab should be removed once");

    // The menu and tab bodies edit the settings they are given.
    ImGui::NewFrame();
    ImGui::Begin("Chrome widgets");
    TransformToolSettings tools;
    ViewportChromeState state;
    ViewportChromeWidgets::drawShadingMenu(state);
    ViewportChromeWidgets::drawOverlaysMenu(state.overlays);
    ViewportChromeWidgets::drawGizmosMenu(tools);
    ImGui::PushID("Shelf");
    ViewportChromeWidgets::drawToolShelf(tools, 60.0f);
    ImGui::PopID();
    ViewportChromeWidgets::drawToolTab(tools);
    ViewportChromeWidgets::drawViewTab(fixture.camera);
    ImGui::End();
    ImGui::Render();
    expect(state.shading == ViewportShading::Rendered && tools.gizmo == GizmoMode::Select,
           "drawing chrome widgets without input should change nothing");
}
} // namespace

int main() {
    testOperators();
    testDefaultsAndPickingIgnoresChrome();
    testKeysToggleRegions();
    testSidebarTabsAndChromeDrawing();
    if (failures != 0) {
        std::cerr << failures << " viewport chrome test(s) failed\n";
        return 1;
    }
    return 0;
}
