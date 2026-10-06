// Viewport picking without a window or GPU: a synthetic pick provider stands
// in for the renderer's id buffer, so clicks, the pending-pick CPU fallback,
// box and lasso selection with their modifiers, Alt+click cycling (visible
// candidates merged with occluded ray hits), the eyedropper, and local view
// all run headless against real cube meshes.

#include "EditorTestFixture.hpp"

#include "editor/document/DefaultScene.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/KeymapDispatch.hpp"
#include "editor/panels/ViewportOperators.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/viewport/BoxSelect.hpp"
#include "editor/viewport/LassoSelect.hpp"
#include "editor/viewport/ViewportPickProvider.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

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

// Runs operators through the fixture's runner and default keymap and records
// eyedropper picks, which the session would hand to ui.eyedropper_pick.
class OperatorActions final : public KeymapSessionActions {
  public:
    using KeymapSessionActions::KeymapSessionActions;

    std::vector<uint64_t> eyedropperPicks;

    OperatorResult invokeOperator(ApplicationContext& context, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override {
        if (operatorId == "ui.eyedropper_pick") {
            eyedropperPicks.push_back(nlohmann::json::parse(properties)["entity"].get<uint64_t>());
            return OperatorResult::finished();
        }
        return KeymapSessionActions::invokeOperator(context, operatorId, properties, region);
    }
};

// A synthetic id buffer covering the whole view: rectangles of view pixels
// carry object ids. Requests resolve after `pollsUntilReady` status queries,
// or never.
class FakePicks final : public ViewportPickProvider {
  public:
    struct Request {
        PickTicket ticket;
        PickRect rect;
        bool cancelled{false};
    };

    bool gpu{true};
    bool neverResolve{false};
    uint32_t pollsUntilReady{1};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint32_t> ids;
    std::vector<float> depth;
    std::vector<PickedObject> objects;
    std::vector<Request> requests;

    void resize(uint32_t viewWidth, uint32_t viewHeight) {
        width = viewWidth;
        height = viewHeight;
        ids.assign(static_cast<size_t>(width) * height, 0);
        depth.assign(ids.size(), 0.0f);
        objects.clear();
    }

    // Draws an object (entity at its render object) as a rectangle of view
    // pixels around center, in front of anything drawn before with lower
    // depth.
    void paint(const Scene& scene, Entity entity, glm::vec2 center, int32_t halfSize, float nearness) {
        objects.push_back(
            {.entity = scene.id(entity), .object = static_cast<RenderObjectId>(entt::to_integral(entity))});
        const uint32_t id = static_cast<uint32_t>(objects.size());
        for (int32_t y = static_cast<int32_t>(center.y) - halfSize; y <= static_cast<int32_t>(center.y) + halfSize;
             ++y) {
            for (int32_t x = static_cast<int32_t>(center.x) - halfSize; x <= static_cast<int32_t>(center.x) + halfSize;
                 ++x) {
                if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= width || static_cast<uint32_t>(y) >= height) {
                    continue;
                }
                const size_t index = static_cast<size_t>(y) * width + static_cast<size_t>(x);
                if (nearness >= depth[index]) {
                    ids[index] = id;
                    depth[index] = nearness;
                }
            }
        }
    }

    bool available(RenderViewId view) const override {
        return gpu && static_cast<bool>(view);
    }

    PickTicket request(RenderViewId, const PickRect& rect) override {
        const PickTicket ticket{static_cast<uint64_t>(requests.size() + 1)};
        requests.push_back({.ticket = ticket, .rect = rect});
        m_polls.push_back(0);
        return ticket;
    }

    PickStatus status(PickTicket ticket) const override {
        const Request& entry = requests[ticket.value - 1];
        if (entry.cancelled) {
            return PickStatus::Cancelled;
        }
        uint32_t& polls = m_polls[ticket.value - 1];
        ++polls;
        return !neverResolve && polls > pollsUntilReady ? PickStatus::Ready : PickStatus::Pending;
    }

    PickResult take(PickTicket ticket) override {
        PickResult result;
        const Request& entry = requests[ticket.value - 1];
        if (entry.cancelled) {
            result.status = PickStatus::Cancelled;
            return result;
        }
        const std::optional<PickRect> region = clampPickRect(entry.rect, width, height);
        result.status = PickStatus::Ready;
        result.region = region.value_or(PickRect{.width = 0, .height = 0});
        result.viewWidth = width;
        result.viewHeight = height;
        result.objects = objects;
        for (uint32_t y = 0; region && y < region->height; ++y) {
            for (uint32_t x = 0; x < region->width; ++x) {
                const size_t index = static_cast<size_t>(region->y + static_cast<int32_t>(y)) * width +
                                     static_cast<size_t>(region->x + static_cast<int32_t>(x));
                result.ids.push_back(ids[index]);
                result.depth.push_back(depth[index]);
            }
        }
        return result;
    }

    void cancel(PickTicket ticket) override {
        requests[ticket.value - 1].cancelled = true;
    }

  private:
    mutable std::vector<uint32_t> m_polls;
};

struct Fixture : EditorTestFixture {
    OperatorActions actions{operators, runner, sessionContextSource()};
    FakePicks picks;
    std::unique_ptr<ViewportPanel> panel;
    // Front: a cube at the origin, one hidden behind it, one to the right, an
    // unselectable one to the left, and one far behind on the right that is
    // hidden in the viewport.
    Entity front{entt::null};
    Entity behind{entt::null};
    Entity right{entt::null};
    Entity locked{entt::null};
    Entity hidden{entt::null};

    Fixture() {
        useActions(actions);
        for (Operator& op : viewportOperators(viewports)) {
            expect(static_cast<bool>(operators.add(std::move(op))), "the viewport operators should register");
        }
        viewports.setPickProvider(&picks);
        panel = std::make_unique<ViewportPanel>(viewports);
        front = cube({0.0f, 0.0f, 0.0f});
        behind = cube({0.0f, 0.0f, -5.0f});
        right = cube({4.0f, 0.0f, 0.0f});
        locked = cube({-4.0f, 0.0f, 0.0f});
        scene.registry().emplace<VisibilityComponent>(locked, VisibilityComponent{.selectable = false});
        hidden = cube({4.0f, 2.5f, 0.0f});
        scene.registry().emplace<VisibilityComponent>(hidden, VisibilityComponent{.hidden = true});
        scene.updateWorldTransforms();
        // Looking along -Z from (0, 0, 10).
        panel->camera().setPivot(glm::vec3(0.0f));
        panel->camera().setDistance(10.0f);
        panel->camera().setViewAngles(0.0f, 0.0f);
    }

    ~Fixture() {
        viewports.setPickProvider(nullptr);
        panel.reset();
    }

    Entity cube(glm::vec3 position) {
        Entity entity = entt::null;
        const SceneResult result = addCube(scene, assets, ZENITH_ASSET_DIR, entity);
        expect(result.success, "the test cube should load");
        if (entity != entt::null) {
            scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
            scene.markTransformDirty(entity);
        }
        return entity;
    }

    // Runs an operator through the session actions, as a viewport does.
    OperatorResult invoke(std::string_view operatorId, std::string_view properties = {}) {
        return actions.invokeOperator(app, operatorId, properties, KeymapRegion::Viewport);
    }

    SceneEntityId id(Entity entity) const {
        return scene.id(entity);
    }

    // Where a world point appears, in view pixels.
    glm::vec2 viewPixel(glm::vec3 world) const {
        const glm::vec2 size = panel->size();
        const RenderView lens = panel->lens(scene);
        const glm::vec4 clip =
            lensProjectionMatrix(lens, size.x / size.y) * lensViewMatrix(lens) * glm::vec4(world, 1.0f);
        return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * size;
    }

    ImVec2 screen(glm::vec2 viewPixel) const {
        return {panel->position().x + viewPixel.x, panel->position().y + viewPixel.y};
    }

    // Paints the synthetic id buffer to match what the camera sees.
    void paintVisibleCubes() {
        const glm::vec2 size = panel->size();
        picks.resize(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y));
        picks.paint(scene, front, viewPixel({0.0f, 0.0f, 0.0f}), 20, 0.5f);
        picks.paint(scene, right, viewPixel({4.0f, 0.0f, 0.0f}), 20, 0.5f);
        picks.paint(scene, locked, viewPixel({-4.0f, 0.0f, 0.0f}), 20, 0.5f);
    }
};

// One ImGui context drawing the fixture's viewport in a full-size window.
class Frames {
  public:
    Frames() {
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

    ~Frames() {
        ImGui::DestroyContext();
    }

    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    void frame(Fixture& fixture, ImVec2 mouse) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        fixture.viewports.beginFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Viewport##picking", nullptr, fixture.panel->windowFlags());
        EditorContext context = fixture.context();
        fixture.panel->update(context, InputState{}, io.DeltaTime);
        fixture.panel->draw(context);
        ImGui::End();
        ImGui::Render();
    }

    void settle(Fixture& fixture, ImVec2 mouse) {
        for (int index = 0; index < 3; ++index) {
            frame(fixture, mouse);
        }
    }

    // Presses (and on the next frame releases) a mouse button at mouse with
    // the given modifiers held during the press.
    void click(Fixture& fixture, ImVec2 mouse, ImGuiKeyChord modifiers = 0,
               ImGuiMouseButton button = ImGuiMouseButton_Left) {
        setModifiers(modifiers);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseButtonEvent(button, true);
        frame(fixture, mouse);
        io.AddMouseButtonEvent(button, false);
        setModifiers(0);
        frame(fixture, mouse);
    }

    // A left drag through points, the modifiers held while releasing.
    void drag(Fixture& fixture, std::span<const ImVec2> points, ImGuiKeyChord releaseModifiers = 0,
              ImGuiMouseButton button = ImGuiMouseButton_Left) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseButtonEvent(button, true);
        frame(fixture, points.front());
        for (size_t index = 1; index < points.size(); ++index) {
            frame(fixture, points[index]);
        }
        setModifiers(releaseModifiers);
        io.AddMouseButtonEvent(button, false);
        frame(fixture, points.back());
        setModifiers(0);
    }

    static void setModifiers(ImGuiKeyChord chord) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, (chord & ImGuiMod_Ctrl) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (chord & ImGuiMod_Shift) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (chord & ImGuiMod_Alt) != 0);
    }
};

ImVec2 corner(const Fixture& fixture) {
    return {fixture.panel->position().x + 2.0f, fixture.panel->position().y + fixture.panel->size().y - 2.0f};
}

void testGpuClick() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    fixture.paintVisibleCubes();
    const glm::vec2 frontPixel = fixture.viewPixel({0.0f, 0.0f, 0.0f});

    frames.click(fixture, fixture.screen(frontPixel));
    expect(fixture.picks.requests.size() == 1, "a click should request one GPU pick");
    if (!fixture.picks.requests.empty()) {
        const PickRect rect = fixture.picks.requests.front().rect;
        constexpr int32_t radius = static_cast<int32_t>(ViewportPicker::clickRadius);
        expect(rect.width == ViewportPicker::clickRadius * 2 + 1 &&
                   rect.x == static_cast<int32_t>(std::floor(frontPixel.x)) - radius,
               "the pick should cover the click's tolerance square");
    }
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.front)},
           "the GPU pick should select the clicked cube a frame later");
    expect(fixture.panel->lastPickOutcome() && fixture.panel->lastPickOutcome()->fromGpu,
           "the click should resolve from the id buffer");

    // Shift extends; a Shift click on nothing keeps the selection; a plain
    // miss clears it.
    frames.click(fixture, fixture.screen(fixture.viewPixel({4.0f, 0.0f, 0.0f})), ImGuiMod_Shift);
    expect(fixture.selection.size() == 2 && fixture.selection.active() == fixture.id(fixture.right),
           "a Shift click should extend the selection");
    frames.click(fixture, fixture.screen({frontPixel.x, 20.0f}), ImGuiMod_Shift);
    expect(fixture.selection.size() == 2, "a Shift click on empty space should keep the selection");
    frames.click(fixture, fixture.screen({frontPixel.x, 20.0f}));
    expect(fixture.selection.empty(), "a click on empty space should clear the selection");

    // The unselectable cube is drawn but never picked.
    frames.click(fixture, fixture.screen(fixture.viewPixel({-4.0f, 0.0f, 0.0f})));
    expect(fixture.selection.empty(), "an unselectable object should not be picked through the id buffer");
}

void testPendingFallsBackToCpu() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    fixture.paintVisibleCubes();
    fixture.picks.neverResolve = true;
    const ImVec2 front = fixture.screen(fixture.viewPixel({0.0f, 0.0f, 0.0f}));

    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frames.frame(fixture, front);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    expect(fixture.panel->pickPending() && fixture.selection.empty(), "a GPU pick should wait for its result");
    for (uint32_t frame = 1; frame < ViewportPicker::frameBudget; ++frame) {
        frames.frame(fixture, front);
    }
    expect(fixture.panel->pickPending(), "the pick should wait for the whole frame budget");
    frames.frame(fixture, front);
    expect(!fixture.panel->pickPending() &&
               fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.front)},
           "a pick still pending after the budget should be answered on the CPU");
    expect(fixture.panel->lastPickOutcome() && !fixture.panel->lastPickOutcome()->fromGpu &&
               !fixture.picks.requests.empty() && fixture.picks.requests.front().cancelled,
           "the late GPU request should be cancelled");

    // Without a GPU the CPU answers within the click's frame; hidden objects
    // are skipped, so a click on the hidden cube's spot misses.
    fixture.picks.gpu = false;
    fixture.selection.clear();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frames.frame(fixture, fixture.screen(fixture.viewPixel({4.0f, 0.0f, 0.0f})));
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frames.frame(fixture, corner(fixture));
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.right)},
           "without a GPU the click should be answered at once");
    frames.click(fixture, fixture.screen(fixture.viewPixel({4.0f, 2.5f, 0.0f})));
    expect(fixture.selection.empty(), "a hidden object should not be picked on the CPU");
    // Hiding the front cube lets the click reach the one behind it.
    fixture.scene.registry().emplace<VisibilityComponent>(fixture.front, VisibilityComponent{.hidden = true});
    frames.click(fixture, fixture.screen(fixture.viewPixel({0.0f, 0.0f, 0.0f})));
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.behind)},
           "a click through a hidden object should pick what is behind it");
}

void testBoxSelect() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    fixture.paintVisibleCubes();
    const glm::vec2 front = fixture.viewPixel({0.0f, 0.0f, 0.0f});
    const glm::vec2 rightCube = fixture.viewPixel({4.0f, 0.0f, 0.0f});
    const glm::vec2 lockedCube = fixture.viewPixel({-4.0f, 0.0f, 0.0f});
    // Around the front and right cubes only.
    const std::vector<ImVec2> frontAndRight{fixture.screen(front - glm::vec2(30.0f)),
                                            fixture.screen(rightCube + glm::vec2(30.0f))};

    expect(static_cast<bool>(fixture.invoke("view3d.select_box")) &&
               fixture.panel->boxSelect().state() == BoxSelect::State::Waiting,
           "view3d.select_box should start the box tool");
    frames.drag(fixture, frontAndRight);
    frames.frame(fixture, frontAndRight.back());
    std::vector<SceneEntityId> selected = fixture.selection.ids();
    std::ranges::sort(selected);
    std::vector<SceneEntityId> expected{fixture.id(fixture.front), fixture.id(fixture.right)};
    std::ranges::sort(expected);
    expect(selected == expected, "a box should select the objects inside it");
    expect(!fixture.panel->selectToolActive(), "the box tool should end with the drag");

    // Ctrl subtracts; Shift extends; the unselectable cube never joins.
    const std::vector<ImVec2> aroundRight{fixture.screen(rightCube - glm::vec2(30.0f)),
                                          fixture.screen(rightCube + glm::vec2(30.0f))};
    fixture.invoke("view3d.select_box");
    frames.drag(fixture, aroundRight, ImGuiMod_Ctrl);
    frames.frame(fixture, aroundRight.back());
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.front)},
           "a box released with Ctrl should subtract");
    const std::vector<ImVec2> everything{fixture.screen(lockedCube - glm::vec2(30.0f)),
                                         fixture.screen(rightCube + glm::vec2(30.0f))};
    fixture.invoke("view3d.select_box");
    frames.drag(fixture, everything, ImGuiMod_Shift);
    frames.frame(fixture, everything.back());
    expect(fixture.selection.size() == 2 && fixture.selection.active() == fixture.id(fixture.front) &&
               !fixture.selection.contains(fixture.id(fixture.locked)),
           "a box released with Shift should extend, skipping unselectable objects");

    // Esc and a right click cancel the waiting tool.
    fixture.invoke("view3d.select_box");
    frames.click(fixture, corner(fixture), 0, ImGuiMouseButton_Right);
    expect(!fixture.panel->selectToolActive() && fixture.selection.size() == 2,
           "a right click should cancel box select");

    // Without a GPU the box falls back to projected object centres.
    fixture.picks.gpu = false;
    fixture.invoke("view3d.select_box", R"({"mode":"set"})");
    frames.drag(fixture, aroundRight);
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.right)},
           "a box without a GPU should select the objects whose centres it encloses");
}

// With no tool started, a left drag in the viewport draws a box (the keymap's
// drag binding), and a plain click still selects.
void testLeftDragSelectsInABox() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    fixture.paintVisibleCubes();
    const glm::vec2 front = fixture.viewPixel({0.0f, 0.0f, 0.0f});
    const glm::vec2 rightCube = fixture.viewPixel({4.0f, 0.0f, 0.0f});
    // From empty space, around the front and right cubes.
    const std::vector<ImVec2> aroundBoth{fixture.screen(front - glm::vec2(30.0f)),
                                         fixture.screen(rightCube + glm::vec2(30.0f))};
    expect(!fixture.panel->selectToolActive(), "no select tool should be running before the drag");
    frames.drag(fixture, aroundBoth);
    frames.frame(fixture, aroundBoth.back());
    std::vector<SceneEntityId> selected = fixture.selection.ids();
    std::ranges::sort(selected);
    std::vector<SceneEntityId> expected{fixture.id(fixture.front), fixture.id(fixture.right)};
    std::ranges::sort(expected);
    expect(selected == expected, "a left drag in empty space should select the objects inside the box");
    expect(!fixture.panel->selectToolActive(), "the drag's box should end with the release");

    // Shift extends what is selected.
    fixture.selection.selectOnly(fixture.id(fixture.front));
    const std::vector<ImVec2> aroundRight{fixture.screen(rightCube - glm::vec2(30.0f)),
                                          fixture.screen(rightCube + glm::vec2(30.0f))};
    // Shift is held from the press, as a user extending a selection does.
    Frames::setModifiers(ImGuiMod_Shift);
    frames.drag(fixture, aroundRight, ImGuiMod_Shift);
    frames.frame(fixture, aroundRight.back());
    expect(fixture.selection.size() == 2 && fixture.selection.contains(fixture.id(fixture.front)) &&
               fixture.selection.contains(fixture.id(fixture.right)),
           "a left drag released with Shift should extend the selection");

    // A click without movement is still a plain select.
    frames.click(fixture, fixture.screen(front));
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.front)} &&
               !fixture.panel->selectToolActive(),
           "a click without a drag should select one object and start no box");
}

void testLassoSelect() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    fixture.paintVisibleCubes();
    const glm::vec2 rightCube = fixture.viewPixel({4.0f, 0.0f, 0.0f});
    const std::vector<ImVec2> triangle{
        fixture.screen(rightCube + glm::vec2(0.0f, -40.0f)), fixture.screen(rightCube + glm::vec2(40.0f, 30.0f)),
        fixture.screen(rightCube + glm::vec2(-40.0f, 30.0f)), fixture.screen(rightCube + glm::vec2(0.0f, -38.0f))};

    expect(static_cast<bool>(fixture.invoke("view3d.select_lasso")) &&
               fixture.panel->lassoSelect().state() == LassoSelect::State::Waiting,
           "view3d.select_lasso should wait for a left drag");
    frames.drag(fixture, triangle);
    frames.frame(fixture, triangle.back());
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.right)},
           "a lasso should select the objects inside its outline");

    // A Ctrl+RMB drag draws with the right button, from where the press
    // started; Ctrl+Shift extends.
    const glm::vec2 front = fixture.viewPixel({0.0f, 0.0f, 0.0f});
    const std::vector<ImVec2> aroundFront{fixture.screen(front + glm::vec2(0.0f, -40.0f)),
                                          fixture.screen(front + glm::vec2(40.0f, 30.0f)),
                                          fixture.screen(front + glm::vec2(-40.0f, 30.0f))};
    Frames::setModifiers(ImGuiMod_Ctrl | ImGuiMod_Shift);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    frames.frame(fixture, aroundFront[0]);
    expect(!fixture.panel->lassoSelect().active(), "a Ctrl+RMB press alone should not start a lasso");
    frames.frame(fixture, aroundFront[1]);
    expect(fixture.panel->lassoSelect().drawsWithRightButton(), "a Ctrl+RMB drag should start a right-button lasso");
    frames.frame(fixture, aroundFront[1]);
    frames.frame(fixture, aroundFront[2]);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    frames.frame(fixture, aroundFront[2]);
    Frames::setModifiers(0);
    frames.frame(fixture, aroundFront[2]);
    expect(fixture.selection.size() == 2 && fixture.selection.contains(fixture.id(fixture.front)),
           "a Ctrl+Shift+RMB lasso should extend the selection");
}

void testCycle() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    fixture.paintVisibleCubes();
    const ImVec2 front = fixture.screen(fixture.viewPixel({0.0f, 0.0f, 0.0f}));

    frames.click(fixture, front, ImGuiMod_Alt);
    frames.frame(fixture, front);
    const ViewportPickOutcome* outcome =
        fixture.panel->lastPickOutcome() ? &*fixture.panel->lastPickOutcome() : nullptr;
    expect(outcome != nullptr && outcome->kind == ViewportPickKind::Cycle && outcome->fromGpu &&
               outcome->entities == std::vector<SceneEntityId>{fixture.id(fixture.front), fixture.id(fixture.behind)},
           "cycle candidates should be the visible object, then the occluded one a ray hits");
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.front)},
           "the first Alt+click should select the front object");
    frames.click(fixture, front, ImGuiMod_Alt);
    frames.frame(fixture, front);
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.behind)},
           "the next Alt+click should select the occluded object");
    frames.click(fixture, front, ImGuiMod_Alt);
    frames.frame(fixture, front);
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.front)},
           "cycling should wrap to the front object");

    // Without a GPU, cycling walks the ray hits.
    fixture.picks.gpu = false;
    frames.click(fixture, front, ImGuiMod_Alt);
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.behind)},
           "CPU cycling should reach the occluded object");
}

// Mouse gestures are keymap bindings: unbound presses stay camera
// navigation, and rebinding the lasso moves the gesture to the new chord.
void testMouseChordBindings() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    fixture.paintVisibleCubes();
    ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 front = fixture.viewPixel({0.0f, 0.0f, 0.0f});
    const ImVec2 start = fixture.screen(front + glm::vec2(0.0f, -40.0f));
    const ImVec2 moved = fixture.screen(front + glm::vec2(40.0f, 30.0f));
    // Returns whether the press frame held the camera back (the test's input
    // state has no buttons down, so the hold ends the next frame).
    const auto rightPress = [&](ImGuiKeyChord modifiers, ImVec2 to) {
        Frames::setModifiers(modifiers);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        frames.frame(fixture, start);
        const bool held = fixture.panel->cameraWaitsForRightRelease();
        frames.frame(fixture, to);
        return held;
    };
    const auto rightRelease = [&] {
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        Frames::setModifiers(0);
        frames.frame(fixture, start);
        frames.frame(fixture, start);
    };

    expect(!fixture.actions.mouseButtonBound(KeymapRegion::Viewport, ImGuiMouseButton_Right),
           "a plain RMB press should not be bound");
    expect(!rightPress(0, start) && !fixture.panel->selectToolActive(),
           "an unbound RMB press should keep fly navigation");
    rightRelease();

    expect(rightPress(ImGuiMod_Ctrl, start) && !fixture.panel->selectToolActive(),
           "a bound Ctrl+RMB press should hold the camera without lassoing until it drags");
    rightRelease();

    const auto lasso = std::ranges::find_if(fixture.actions.keymap.bindings(), [](const KeymapBinding& binding) {
        return binding.operatorId == "view3d.select_lasso" && binding.chord == (ImGuiMod_Ctrl | ImGuiKey_MouseRight);
    });
    expect(lasso != fixture.actions.keymap.bindings().end(), "Ctrl+RMB drag should be the default lasso");
    const auto lassoIndex = static_cast<size_t>(lasso - fixture.actions.keymap.bindings().begin());
    expect(fixture.actions.keymap.rebind(lassoIndex, ImGuiMod_Alt | ImGuiKey_MouseRight).success,
           "the lasso should rebind to Alt+RMB");
    expect(!rightPress(ImGuiMod_Ctrl, moved) && !fixture.panel->selectToolActive(),
           "the old chord should navigate once the lasso moved away");
    rightRelease();
    rightPress(ImGuiMod_Alt, moved);
    expect(fixture.panel->lassoSelect().drawsWithRightButton(), "an Alt+RMB drag should start the rebound lasso");
    rightRelease();
}

void testEyedropperTakesTheClick() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    fixture.paintVisibleCubes();
    fixture.selection.selectOnly(fixture.id(fixture.right));
    fixture.eyedropper.arm({.entity = fixture.id(fixture.right), .componentKey = "test", .fieldPath = {"target"}},
                           &fixture.document);
    const ImVec2 front = fixture.screen(fixture.viewPixel({0.0f, 0.0f, 0.0f}));
    frames.click(fixture, front);
    expect(fixture.actions.eyedropperPicks == std::vector<uint64_t>{fixture.id(fixture.front).value},
           "an armed eyedropper should take the GPU-picked entity");
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.right)},
           "the eyedropper click should leave the selection alone");
    fixture.picks.gpu = false;
    frames.click(fixture, fixture.screen({10.0f, 30.0f}));
    expect(fixture.actions.eyedropperPicks.size() == 1 && fixture.selection.size() == 1,
           "an eyedropper click on empty space should change nothing");
}

void testLocalView() {
    Fixture fixture;
    Frames frames;
    frames.settle(fixture, corner(fixture));
    const glm::vec3 pivotBefore = fixture.panel->camera().pivot();
    const float distanceBefore = fixture.panel->camera().distance();

    expect(!fixture.invoke("view3d.localview"), "local view needs a selection");
    fixture.selection.selectOnly(fixture.id(fixture.right));
    expect(static_cast<bool>(fixture.invoke("view3d.localview")) && fixture.panel->localViewActive(),
           "view3d.localview should enter local view");
    frames.frame(fixture, corner(fixture));
    const std::vector<RenderObjectId> rightObject{static_cast<RenderObjectId>(entt::to_integral(fixture.right))};
    expect(fixture.rendering.viewVisibleEntities(fixture.panel->renderView()) == rightObject,
           "local view should isolate the selection in the viewport's render view");
    expect(glm::length(fixture.panel->camera().pivot() - glm::vec3(4.0f, 0.0f, 0.0f)) < 0.01f,
           "local view should frame the isolated objects");

    // Picks only consider the isolated set, on the GPU and the CPU.
    fixture.paintVisibleCubes();
    fixture.selection.clear();
    frames.click(fixture, fixture.screen(fixture.viewPixel({0.0f, 0.0f, 0.0f})));
    expect(fixture.selection.empty(), "a GPU pick in local view should ignore objects outside it");
    frames.click(fixture, fixture.screen(fixture.viewPixel({4.0f, 0.0f, 0.0f})));
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.id(fixture.right)},
           "a GPU pick in local view should reach the isolated object");
    fixture.picks.gpu = false;
    fixture.selection.clear();
    frames.click(fixture, fixture.screen(fixture.viewPixel({0.0f, 0.0f, 0.0f})));
    expect(fixture.selection.empty(), "a CPU pick in local view should ignore objects outside it");

    expect(static_cast<bool>(fixture.invoke("view3d.localview")) && !fixture.panel->localViewActive(),
           "view3d.localview should leave local view");
    frames.frame(fixture, corner(fixture));
    expect(!fixture.rendering.viewVisibleEntities(fixture.panel->renderView()),
           "leaving local view should show everything again");
    expect(glm::length(fixture.panel->camera().pivot() - pivotBefore) < 1e-4f &&
               std::abs(fixture.panel->camera().distance() - distanceBefore) < 1e-4f,
           "leaving local view should restore the camera");
}

void testGestureTools() {
    BoxSelect box;
    box.begin(SelectionSetMode::Auto);
    SelectionGestureInput input{.mouse = {10.0f, 10.0f}, .hovered = false, .leftPressed = true, .leftDown = true};
    expect(!box.update(input) && box.state() == BoxSelect::State::Waiting,
           "a press outside the view image should not start the box");
    input.hovered = true;
    box.update(input);
    input = {.mouse = {50.0f, 40.0f}, .hovered = true, .leftDown = true};
    expect(!box.update(input) && box.state() == BoxSelect::State::Dragging, "the box should follow the drag");
    input = {.mouse = {60.0f, 45.0f}, .hovered = true, .ctrl = true};
    const std::optional<RegionSelection> finished = box.update(input);
    expect(finished && finished->mode == SelectionSetMode::Subtract && regionMinimum(*finished) == glm::vec2(10.0f) &&
               regionMaximum(*finished) == glm::vec2(60.0f, 45.0f),
           "releasing with Ctrl should finish a subtracting box");
    box.begin(SelectionSetMode::Set);
    expect(!box.update({.escapePressed = true}) && !box.active(), "Esc should cancel the box");

    LassoSelect lasso;
    lasso.begin(SelectionSetMode::Set, true);
    lasso.update({.mouse = {0.0f, 0.0f}, .rightDown = true});
    lasso.update({.mouse = {0.5f, 0.5f}, .rightDown = true});
    expect(!lasso.update({.mouse = {1.0f, 0.0f}}) && !lasso.active(),
           "a lasso of fewer than three distinct points should select nothing");

    Selection selection;
    selection.assign({SceneEntityId{1}, SceneEntityId{2}});
    const std::vector<SceneEntityId> enclosed{SceneEntityId{3}, SceneEntityId{2}};
    applySelectionSet(selection, enclosed, SelectionSetMode::Set);
    expect(selection.ids() == std::vector<SceneEntityId>{SceneEntityId{2}, SceneEntityId{3}} ||
               selection.ids() == std::vector<SceneEntityId>{SceneEntityId{3}, SceneEntityId{2}},
           "Set should select exactly the enclosed entities");
    selection.assign({SceneEntityId{1}, SceneEntityId{2}});
    applySelectionSet(selection, enclosed, SelectionSetMode::Extend);
    expect(selection.ids() == std::vector<SceneEntityId>{SceneEntityId{1}, SceneEntityId{2}, SceneEntityId{3}},
           "Extend should add new entities after the current selection");
    applySelectionSet(selection, enclosed, SelectionSetMode::Subtract);
    expect(selection.ids() == std::vector<SceneEntityId>{SceneEntityId{1}}, "Subtract should remove them");
}
} // namespace

int main() {
    testGestureTools();
    testGpuClick();
    testPendingFallsBackToCpu();
    testBoxSelect();
    testLeftDragSelectsInABox();
    testLassoSelect();
    testCycle();
    testMouseChordBindings();
    testEyedropperTakesTheClick();
    testLocalView();
    if (failures == 0) {
        std::cout << "Viewport picking tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
