#include "EditorTestFixture.hpp"

#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/KeymapDispatch.hpp"
#include "editor/panels/ViewportOperators.hpp"
#include "editor/panels/ViewportPanel.hpp"

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
// through the fixture's registry and runner with the default keymap, and
// region keys are dispatched the way the session does.
class HeadlessSessionActions final : public KeymapSessionActions {
  public:
    using KeymapSessionActions::KeymapSessionActions;

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
};

// Everything EditorContext references, owned together.
struct ViewportFixture : EditorTestFixture {
    HeadlessSessionActions actions{operators, runner, sessionContextSource()};
    ViewportPanel viewport{viewports};

    ViewportFixture() {
        useActions(actions);
        for (Operator& op : viewportOperators(viewports)) {
            expect(static_cast<bool>(operators.add(std::move(op))), "the viewport operators should register");
        }
    }

    // Runs an operator through the session actions, as a button does.
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

void testOverlayOperatorsAndAreaState() {
    ViewportFixture fixture;
    ViewportOverlays& overlays = fixture.viewport.chrome().overlays;
    expect(overlays.scene == SceneGizmoOverlays{} && !overlays.sceneInPlay,
           "a new viewport starts with the default scene overlays, off in Play");
    expect(static_cast<bool>(fixture.invoke("view3d.overlay_toggle", R"({"overlay":"grid"})")) && !overlays.grid,
           "view3d.overlay_toggle should flip an on/off overlay");
    expect(static_cast<bool>(fixture.invoke("view3d.overlay_toggle", R"({"overlay":"lights"})")) &&
               overlays.scene.lights == OverlayScope::Off,
           "toggling a shown scoped overlay should turn it off");
    expect(static_cast<bool>(fixture.invoke("view3d.overlay_toggle", R"({"overlay":"colliders"})")) &&
               overlays.scene.colliders == OverlayScope::Off &&
               static_cast<bool>(fixture.invoke("view3d.overlay_toggle", R"({"overlay":"colliders"})")) &&
               overlays.scene.colliders == SceneGizmoOverlays{}.colliders,
           "toggling a scoped overlay back on should restore its default scope");
    expect(static_cast<bool>(fixture.invoke("view3d.overlay_scope_set", R"({"overlay":"colliders","scope":"all"})")) &&
               overlays.scene.colliders == OverlayScope::All,
           "view3d.overlay_scope_set should set a scoped overlay");
    expect(fixture.invoke("view3d.overlay_scope_set", R"({"overlay":"grid","scope":"all"})").status ==
               OperatorStatus::Failed,
           "an on/off overlay should refuse a scope");
    expect(fixture.invoke("view3d.overlay_toggle", R"({"overlay":"x-ray"})").status == OperatorStatus::Failed,
           "an unknown overlay should fail");
    expect(static_cast<bool>(fixture.invoke("view3d.overlay_toggle", R"({"overlay":"scene_in_play"})")) &&
               overlays.sceneInPlay,
           "Play-mode scene overlays are toggled like any overlay");
    expect(!fixture.history.canUndo(), "overlay operators should not record undo steps");

    expect(overlays.navigationMesh &&
               static_cast<bool>(fixture.invoke("view3d.overlay_toggle", R"({"overlay":"navigation_mesh"})")) &&
               !overlays.navigationMesh,
           "the navigation mesh preview is shown by default and toggled like any overlay");
    expect(!overlays.physicsShapes && !overlays.contacts && !overlays.characters && !overlays.navAgents,
           "Play's debugging overlays start off");
    for (const char* overlay : {"physics_shapes", "contacts", "characters", "nav_agents"}) {
        expect(static_cast<bool>(
                   fixture.invoke("view3d.overlay_toggle", std::string(R"({"overlay":")") + overlay + R"("})")),
               "Play's debugging overlays toggle like any overlay");
    }
    expect(overlays.physicsShapes && overlays.contacts && overlays.characters && overlays.navAgents,
           "each debugging overlay turned on");

    // The overlays travel with the area.
    overlays.scene.names = OverlayScope::All;
    overlays.scene.selectionBounds = false;
    const std::string saved = fixture.viewport.areaState();
    ViewportRegistry otherRegistry;
    ViewportPanel restored(otherRegistry);
    restored.restoreAreaState(saved);
    const ViewportOverlays& copy = restored.chrome().overlays;
    expect(copy.scene == overlays.scene && copy.grid == overlays.grid && copy.sceneInPlay == overlays.sceneInPlay &&
               copy.gizmo == overlays.gizmo && copy.navigationMesh == overlays.navigationMesh && copy.physicsShapes &&
               copy.contacts && copy.characters && copy.navAgents,
           "a viewport's area state should round-trip its overlays");
    ViewportPanel defaults(otherRegistry);
    defaults.restoreAreaState("not json");
    defaults.restoreAreaState(R"({"overlays":{"lights":"sideways","grid":3,"unknown":true}})");
    expect(defaults.chrome().overlays.scene == SceneGizmoOverlays{} && defaults.chrome().overlays.grid,
           "unreadable area state should keep the defaults");

    // Headless frames record nothing visible but must run with every overlay.
    HeadlessImGui imgui;
    for (const char* overlay : {"lights", "cameras", "reflection_probes", "decals", "irradiance_volumes", "names"}) {
        fixture.invoke("view3d.overlay_scope_set", std::string(R"({"overlay":")") + overlay + R"(","scope":"all"})");
    }
    selectNewEntity(fixture);
    imgui.settle(fixture, {10.0f, 10.0f});
}

void testSidebarTabsAndChromeDrawing() {
    ViewportFixture fixture;
    HeadlessImGui imgui;
    ViewportPanel& viewport = fixture.viewport;
    const auto drawTab = [](EditorContext&) { ImGui::TextUnformatted("Module tab"); };
    ViewportRegistry& viewports = fixture.viewports;
    expect(static_cast<bool>(viewports.addSidebarTab({.title = "Spinners", .draw = drawTab})),
           "a module tab should be added");
    expect(!viewports.addSidebarTab({.title = "Spinners", .draw = drawTab}), "duplicate tab titles should be refused");
    expect(!viewports.addSidebarTab({.title = "View", .draw = drawTab}), "built-in tab titles should be refused");
    expect(!viewports.addSidebarTab({.title = "", .draw = drawTab}), "tabs need a title");
    expect(!viewports.addSidebarTab({.title = "Empty"}), "tabs need a draw function");

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
    expect(viewports.removeSidebarTab("Spinners") && !viewports.removeSidebarTab("Spinners"),
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
    ViewportChromeWidgets::drawViewTab(fixture.viewport.camera());
    ImGui::End();
    ImGui::Render();
    expect(state.shading == ViewportShading::Rendered && tools.gizmo == GizmoMode::Select,
           "drawing chrome widgets without input should change nothing");
}
} // namespace

int main() {
    testOperators();
    testOverlayOperatorsAndAreaState();
    testDefaultsAndPickingIgnoresChrome();
    testKeysToggleRegions();
    testSidebarTabsAndChromeDrawing();
    if (failures != 0) {
        std::cerr << failures << " viewport chrome test(s) failed\n";
        return 1;
    }
    return 0;
}
