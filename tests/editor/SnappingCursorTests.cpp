// Snapping, pivot modes, and the 3D cursor without a window or GPU: modal
// transform math for every pivot and snap mode, CPU snap queries against real
// cube meshes, the cursor and pivot operators driven through a viewport in an
// ImGui frame, spawning at the cursor (add operators, Adjust Last Operation,
// model drops, prefab instances), the cursor's drawing, the viewport area
// state, and the cursor's per-scene sidecar.

#include "EditorTestFixture.hpp"

#include "editor/document/DefaultScene.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/KeymapDispatch.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "editor/panels/ViewportOperators.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/viewport/Cursor3D.hpp"
#include "editor/viewport/SnapQueries.hpp"
#include "engine/graphics/DebugDraw.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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

bool near(glm::vec3 left, glm::vec3 right, float tolerance = 0.001f) {
    return glm::length(left - right) <= tolerance;
}

bool near(float left, float right, float tolerance = 0.001f) {
    return std::abs(left - right) <= tolerance;
}

glm::mat4 at(glm::vec3 position) {
    return glm::translate(glm::mat4(1.0f), position);
}

glm::vec3 position(const glm::mat4& world) {
    return glm::vec3(world[3]);
}

std::filesystem::path freshDirectory(const std::string& name) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("zenith-snapping-tests-" + name);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

// --- Modal transform math -------------------------------------------------

// A camera at +Z looking down -Z, matching the editor's Vulkan projection.
ModalTransformView frontView() {
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 800.0f / 600.0f, 0.1f, 1000.0f);
    projection[1][1] *= -1.0f;
    return {.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
            .projection = projection,
            .viewportPosition = {0.0f, 0.0f},
            .viewportSize = {800.0f, 600.0f}};
}

ModalTransform typed(TransformOperation operation, const std::vector<glm::mat4>& world, ModalTransformPivot pivot,
                     const char* value) {
    ModalTransform transform(operation, world, glm::mat3(1.0f), {0.0f, 0.0f}, frontView(), pivot);
    for (const char* character = value; *character != '\0'; ++character) {
        transform.typeCharacter(*character);
    }
    return transform;
}

// Starts a move with the mouse on the moved objects' reference point.
ModalTransform grab(const std::vector<glm::mat4>& world, ModalTransformPivot pivot = {}) {
    const ModalTransform probe(TransformOperation::Translate, world, glm::mat3(1.0f), {0.0f, 0.0f}, frontView(), pivot);
    return ModalTransform(TransformOperation::Translate, world, glm::mat3(1.0f), probe.pivotOnScreen(), frontView(),
                          pivot);
}

void testPivotModes() {
    const std::vector<glm::mat4> two{at({1.0f, 0.0f, 0.0f}), at({3.0f, 0.0f, 0.0f})};
    const ModalTransformPivot median{};
    const ModalTransformPivot active{.mode = TransformPivot::ActiveElement,
                                     .activeOrigin = glm::vec3(3.0f, 0.0f, 0.0f)};
    const ModalTransformPivot individual{.mode = TransformPivot::IndividualOrigins};
    const ModalTransformPivot cursor{.mode = TransformPivot::Cursor, .cursor = glm::vec3(0.0f)};

    // Typed 90 degrees turns counter-clockwise about +Z, toward the viewer.
    std::vector<glm::mat4> world = typed(TransformOperation::Rotate, two, median, "90").evaluate({}).world;
    expect(near(position(world[0]), {2.0f, -1.0f, 0.0f}) && near(position(world[1]), {2.0f, 1.0f, 0.0f}),
           "Median Point should rotate both objects about their median");
    world = typed(TransformOperation::Rotate, two, active, "90").evaluate({}).world;
    expect(near(position(world[0]), {3.0f, -2.0f, 0.0f}) && near(position(world[1]), {3.0f, 0.0f, 0.0f}),
           "Active Element should rotate about the active object's origin");
    world = typed(TransformOperation::Rotate, two, individual, "90").evaluate({}).world;
    expect(near(position(world[0]), {1.0f, 0.0f, 0.0f}) && near(position(world[1]), {3.0f, 0.0f, 0.0f}) &&
               near(glm::vec3(world[0][0]), {0.0f, 1.0f, 0.0f}) && near(glm::vec3(world[1][0]), {0.0f, 1.0f, 0.0f}),
           "Individual Origins should turn each object in place");
    world = typed(TransformOperation::Rotate, two, cursor, "90").evaluate({}).world;
    expect(near(position(world[0]), {0.0f, 1.0f, 0.0f}) && near(position(world[1]), {0.0f, 3.0f, 0.0f}),
           "3D Cursor should rotate about the cursor");

    world = typed(TransformOperation::Scale, two, median, "2").evaluate({}).world;
    expect(near(position(world[0]), {0.0f, 0.0f, 0.0f}) && near(position(world[1]), {4.0f, 0.0f, 0.0f}),
           "Median Point should scale away from the median");
    world = typed(TransformOperation::Scale, two, active, "2").evaluate({}).world;
    expect(near(position(world[0]), {-1.0f, 0.0f, 0.0f}) && near(position(world[1]), {3.0f, 0.0f, 0.0f}),
           "Active Element should scale away from the active origin");
    world = typed(TransformOperation::Scale, two, individual, "2").evaluate({}).world;
    expect(near(position(world[0]), {1.0f, 0.0f, 0.0f}) && near(position(world[1]), {3.0f, 0.0f, 0.0f}) &&
               near(glm::length(glm::vec3(world[0][0])), 2.0f) && near(glm::length(glm::vec3(world[1][1])), 2.0f),
           "Individual Origins should scale each object in place");
    world = typed(TransformOperation::Scale, two, cursor, "2").evaluate({}).world;
    expect(near(position(world[0]), {2.0f, 0.0f, 0.0f}) && near(position(world[1]), {6.0f, 0.0f, 0.0f}),
           "3D Cursor should scale away from the cursor");

    // Moving ignores the pivot point, except Active Element's reference.
    const ModalTransformPivot farCursor{.mode = TransformPivot::Cursor, .cursor = glm::vec3(9.0f)};
    expect(near(grab(two, farCursor).pivot(), {2.0f, 0.0f, 0.0f}) &&
               near(grab(two, active).pivot(), {3.0f, 0.0f, 0.0f}),
           "moving should work about the median, or the active origin with Active Element");
    expect(near(transformPivotPoint(farCursor, {}), glm::vec3(9.0f)) &&
               near(transformPivotPoint({.mode = TransformPivot::ActiveElement}, two), {2.0f, 0.0f, 0.0f}),
           "the cursor pivot needs no objects and Active Element without an active object uses the median");
}

void testIncrementSnapping() {
    // 37 pixels at ten meters is about 0.71 m.
    const auto moved = [](const TransformSnapping& snapping) {
        ModalTransform transform = grab({at({0.3f, 0.0f, 0.0f})});
        transform.pressAxis(TransformAxis::X, false);
        transform.moveMouse(transform.pivotOnScreen() + glm::vec2(37.0f, 0.0f), false);
        return position(transform.evaluate(snapping).world.front());
    };
    const glm::vec3 free = moved({});
    const glm::vec3 relative = moved({.enabled = true, .translation = 1.0f});
    const glm::vec3 absolute = moved({.enabled = true, .translation = 1.0f, .absoluteGrid = true});
    expect(free.x > 0.8f && !near(free.x, std::round(free.x)), "the unsnapped move should land between increments");
    expect(near(relative.x, 1.3f) && near(relative.y, 0.0f) && near(relative.z, 0.0f),
           "relative increments should move by whole steps from the start");
    expect(near(absolute.x, 1.0f) && near(absolute.y, 0.0f) && near(absolute.z, 0.0f),
           "absolute grid snapping should land the origin on the grid along the free axis only");

    // An unconstrained absolute move snaps every view-plane axis, keeping
    // the object's depth on the grid it started on.
    ModalTransform plane = grab({at({0.3f, 0.2f, 0.0f})});
    plane.moveMouse(plane.pivotOnScreen() + glm::vec2(37.0f, -61.0f), false);
    const glm::vec3 snapped =
        position(plane.evaluate({.enabled = true, .translation = 0.5f, .absoluteGrid = true}).world.front());
    expect(near(snapped.x, std::round(snapped.x * 2.0f) / 2.0f) && near(snapped.y, std::round(snapped.y * 2.0f) / 2.0f),
           "an unconstrained absolute move should snap both screen axes to the grid");
}

void testTargetSnapping() {
    const glm::vec3 target(5.0f, 2.0f, -1.0f);
    const TransformSnapping surface{.enabled = true, .target = SnapTarget::Surface, .targetPoint = target};

    ModalTransform freeMove = grab({at({0.0f, 0.0f, 0.0f})});
    freeMove.moveMouse(freeMove.pivotOnScreen() + glm::vec2(20.0f, 0.0f), false);
    const ModalTransformResult result = freeMove.evaluate(surface);
    expect(near(position(result.world.front()), target) && result.snappedTo && near(*result.snappedTo, target),
           "surface snapping should put the origin on the target and report it");

    ModalTransform axis = grab({at({0.0f, 0.0f, 0.0f})});
    axis.pressAxis(TransformAxis::X, false);
    expect(near(position(axis.evaluate(surface).world.front()), {5.0f, 0.0f, 0.0f}),
           "an axis constraint should project the target onto the axis");
    ModalTransform plane = grab({at({0.0f, 0.0f, 0.0f})});
    plane.pressAxis(TransformAxis::Z, true);
    expect(near(position(plane.evaluate(surface).world.front()), {5.0f, 2.0f, 0.0f}),
           "a plane constraint should project the target onto the plane");

    ModalTransform missing = grab({at({0.0f, 0.0f, 0.0f})});
    missing.moveMouse(missing.pivotOnScreen() + glm::vec2(20.0f, 0.0f), false);
    const ModalTransformResult unsnapped = missing.evaluate({});
    const ModalTransformResult noTarget =
        missing.evaluate({.enabled = true, .translation = 1.0f, .target = SnapTarget::Vertex});
    expect(near(position(noTarget.world.front()), position(unsnapped.world.front())) && !noTarget.snappedTo,
           "without a target under the mouse the move should stay free, not snap in increments");

    // The median of two objects lands on the target, keeping their offsets.
    ModalTransform pair = grab({at({0.0f, 0.0f, 0.0f}), at({2.0f, 0.0f, 0.0f})});
    const std::vector<glm::mat4> world = pair.evaluate(surface).world;
    expect(near(position(world[0]), {4.0f, 2.0f, -1.0f}) && near(position(world[1]), {6.0f, 2.0f, -1.0f}),
           "the selection's median should snap, keeping the objects' offsets");

    // Rotation still snaps in increments with a surface target.
    ModalTransform rotate(TransformOperation::Rotate, {at({1.0f, 0.0f, 0.0f})}, glm::mat3(1.0f), {0.0f, 0.0f},
                          frontView());
    expect(!rotate.evaluate(surface).snappedTo, "rotation should not report a snap target");

    TransformToolSettings tools;
    tools.snapTarget = SnapTarget::Vertex;
    tools.absoluteGridSnap = true;
    const TransformSnapping held = transformSnapping(tools, true);
    expect(held.enabled && held.target == SnapTarget::Vertex && held.absoluteGrid && !held.targetPoint,
           "Ctrl should turn snapping on while the tool's snapping is off");
    tools.snapping = true;
    expect(!transformSnapping(tools, true).enabled && transformSnapping(tools, false).enabled,
           "Ctrl should turn snapping off while the tool's snapping is on");
}

// --- Viewport fixture -----------------------------------------------------

// A viewport over two cubes in a fresh asset root. Mouse chords run through
// the default keymap, as in the session, so Shift+RMB reaches
// view3d.cursor3d_place.
struct Fixture : EditorTestFixture {
    std::filesystem::path root;
    KeymapSessionActions actions{operators, runner, sessionContextSource()};
    std::unique_ptr<ViewportPanel> panel;
    // A cube at the origin and one five meters behind it.
    Entity front{entt::null};
    Entity behind{entt::null};
    // Half the default cube's size.
    float halfExtent{0.5f};

    explicit Fixture(const std::string& name)
        : EditorTestFixture({.documentRoot = freshDirectory(name)})
        , root(document.options().assetRoot) {
        useActions(actions);
        // Every asset lives under the document's root, so undo snapshots
        // can name them.
        std::filesystem::create_directories(root / defaultCubeModel.parent_path());
        std::filesystem::copy_file(std::filesystem::path(ZENITH_ASSET_DIR) / defaultCubeModel, root / defaultCubeModel);
        for (Operator& op : viewportOperators(viewports)) {
            expect(static_cast<bool>(operators.add(std::move(op))), "the viewport operators should register");
        }
        for (Operator& op : cursorOperators(viewports, document)) {
            expect(static_cast<bool>(operators.add(std::move(op))), "the cursor operators should register");
        }
        expect(registerObjectOperators(operators).success && registerPrefabOperators(operators).success,
               "the object and prefab operators should register");
        panel = std::make_unique<ViewportPanel>(viewports);
        front = cube({0.0f, 0.0f, 0.0f});
        behind = cube({0.0f, 0.0f, -5.0f});
        if (front != entt::null) {
            const MeshAsset* mesh = assets.mesh(scene.registry().get<MeshRendererComponent>(front).mesh);
            if (mesh != nullptr) {
                halfExtent = 0.0f;
                for (const Vertex& vertex : mesh->vertices) {
                    halfExtent = std::max(halfExtent, std::abs(vertex.position.z));
                }
            }
        }
        scene.updateWorldTransforms();
        // Looking along -Z from (0, 0, 10).
        panel->camera().setPivot(glm::vec3(0.0f));
        panel->camera().setDistance(10.0f);
        panel->camera().setViewAngles(0.0f, 0.0f);
    }

    ~Fixture() {
        panel.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    Entity cube(glm::vec3 location) {
        Entity entity = entt::null;
        const SceneResult result = addCube(scene, assets, root, entity);
        expect(result.success, "the test cube should load");
        if (entity != entt::null) {
            scene.registry().get<TransformComponent>(entity).local = at(location);
            scene.markTransformDirty(entity);
        }
        return entity;
    }

    // Runs an operator through the session actions, as a viewport does.
    OperatorResult invoke(std::string_view operatorId, std::string_view properties = {}) {
        return actions.invokeOperator(app, operatorId, properties, KeymapRegion::Viewport);
    }

    glm::vec3& cursor() {
        return document.editorData().cursorLocation;
    }

    glm::vec3 worldPosition(Entity entity) {
        scene.updateWorldTransforms();
        return position(scene.registry().get<TransformComponent>(entity).world);
    }

    glm::vec3 activePosition() {
        const std::optional<SceneEntityId> active = selection.active();
        const Entity entity = active ? scene.findEntity(*active) : entt::null;
        return entity == entt::null ? glm::vec3(-999.0f) : worldPosition(entity);
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

    std::string placeAt(glm::vec2 viewPixel) const {
        return nlohmann::json{{"x", viewPixel.x}, {"y", viewPixel.y}}.dump();
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
        ImGui::Begin("Viewport##snapping", nullptr, fixture.panel->windowFlags());
        EditorContext context = fixture.context();
        fixture.panel->update(context, InputState{}, io.DeltaTime);
        fixture.panel->draw(context);
        ImGui::End();
        ImGui::Render();
    }

    void settle(Fixture& fixture) {
        for (int index = 0; index < 3; ++index) {
            frame(fixture, {2.0f, 700.0f});
        }
    }
};

// --- Snap queries ---------------------------------------------------------

void testSnapQueries() {
    Fixture fixture("queries");
    Frames frames;
    frames.settle(fixture);
    const SnapView view = fixture.panel->snapView(fixture.scene);
    const float half = fixture.halfExtent;
    const glm::vec2 center = fixture.viewPixel({0.0f, 0.0f, 0.0f});

    const std::optional<glm::vec3> surface = surfaceSnapPoint(fixture.scene, fixture.assets, view, center);
    expect(surface && near(*surface, {0.0f, 0.0f, half}), "surface snapping should hit the front cube's face");
    const std::vector<Entity> excludeFront =
        snapExclusions(fixture.scene, std::vector<SceneEntityId>{fixture.scene.id(fixture.front)});
    const std::optional<glm::vec3> pastFront =
        surfaceSnapPoint(fixture.scene, fixture.assets, view, center, excludeFront);
    expect(pastFront && near(*pastFront, {0.0f, 0.0f, -5.0f + half}),
           "surface snapping should look past the objects being moved");
    expect(!surfaceSnapPoint(fixture.scene, fixture.assets, view, {3.0f, 3.0f}),
           "surface snapping over empty space should find nothing");

    const glm::vec3 corner(half, half, half);
    const glm::vec2 cornerPixel = fixture.viewPixel(corner);
    const std::optional<glm::vec3> vertex =
        vertexSnapPoint(fixture.scene, fixture.assets, view, cornerPixel + glm::vec2(4.0f, -3.0f), 24.0f);
    expect(vertex && near(*vertex, corner), "vertex snapping should find the corner nearest the mouse");
    // Just outside the corner, nearer to it on screen than to any other
    // vertex (the back face's corner projects a little inward).
    const std::optional<glm::vec3> nearest =
        vertexSnapPoint(fixture.scene, fixture.assets, view, cornerPixel + glm::vec2(3.0f, -3.0f), 1000.0f);
    expect(nearest && near(*nearest, corner), "vertex snapping should prefer the vertex nearest on screen");
    const std::optional<glm::vec3> pastFrontVertex =
        vertexSnapPoint(fixture.scene, fixture.assets, view, cornerPixel, 80.0f, excludeFront);
    expect(pastFrontVertex && pastFrontVertex->z < -5.0f + half + 0.01f,
           "vertex snapping should skip the objects being moved");
    expect(!vertexSnapPoint(fixture.scene, fixture.assets, view, {3.0f, 3.0f}, 24.0f),
           "vertex snapping far from every mesh should find nothing");
    expect(!snapTargetPoint(SnapTarget::Increment, fixture.scene, fixture.assets, view, center, {}) &&
               snapTargetPoint(SnapTarget::Vertex, fixture.scene, fixture.assets, view, cornerPixel, {}),
           "snapTargetPoint should dispatch on the target");

    // Hidden objects are no targets.
    fixture.scene.registry().emplace<VisibilityComponent>(fixture.front, VisibilityComponent{.hidden = true});
    const std::optional<glm::vec3> throughHidden = surfaceSnapPoint(fixture.scene, fixture.assets, view, center);
    expect(throughHidden && near(*throughHidden, {0.0f, 0.0f, -5.0f + half}),
           "surface snapping should ignore hidden objects");
}

// --- Cursor operators -----------------------------------------------------

void testCursorPlacement() {
    Fixture fixture("placement");
    Frames frames;
    frames.settle(fixture);
    const float half = fixture.halfExtent;
    const glm::vec2 center = fixture.viewPixel({0.0f, 0.0f, 0.0f});

    expect(static_cast<bool>(fixture.invoke("view3d.cursor3d_place", fixture.placeAt(center))) &&
               near(fixture.cursor(), {0.0f, 0.0f, half}),
           "view3d.cursor3d_place should put the cursor on the surface under the point");
    expect(static_cast<bool>(fixture.invoke("view3d.cursor3d_place", fixture.placeAt({10.0f, 10.0f}))) &&
               near(fixture.cursor().z, half) && fixture.cursor().x < 0.0f && fixture.cursor().y > 0.0f,
           "over empty space the cursor should keep its depth along the view ray");
    const uint64_t revision = fixture.history.revision();
    expect(static_cast<bool>(fixture.invoke("view3d.cursor_reset")) && near(fixture.cursor(), glm::vec3(0.0f)),
           "view3d.cursor_reset should return the cursor to the origin");
    expect(fixture.history.revision() == revision, "moving the cursor should not record an undo step");

    // The Shift+RMB gesture places the cursor at the mouse.
    fixture.cursor() = glm::vec3(7.0f);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    frames.frame(fixture, fixture.screen(center));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    frames.frame(fixture, fixture.screen(center));
    // ImGui floors the mouse position, so the hit is within a pixel.
    expect(near(fixture.cursor(), {0.0f, 0.0f, half}, 0.05f), "Shift+RMB should place the cursor on the surface");

    // Cursor to Selected, then Selection to Cursor with and without offsets.
    fixture.selection.assign({fixture.scene.id(fixture.front), fixture.scene.id(fixture.behind)});
    expect(static_cast<bool>(fixture.invoke("view3d.snap_cursor_to_selected")) &&
               near(fixture.cursor(), {0.0f, 0.0f, -2.5f}),
           "view3d.snap_cursor_to_selected should move the cursor to the selection's median");
    fixture.cursor() = glm::vec3(1.0f, 2.0f, 3.0f);
    expect(static_cast<bool>(fixture.invoke("view3d.snap_selected_to_cursor")) &&
               near(fixture.worldPosition(fixture.front), {1.0f, 2.0f, 3.0f}) &&
               near(fixture.worldPosition(fixture.behind), {1.0f, 2.0f, 3.0f}),
           "view3d.snap_selected_to_cursor should move every selected object to the cursor");
    fixture.editing().undo();
    expect(near(fixture.worldPosition(fixture.front), glm::vec3(0.0f)) &&
               near(fixture.worldPosition(fixture.behind), {0.0f, 0.0f, -5.0f}),
           "undo should put the snapped objects back");
    expect(static_cast<bool>(fixture.invoke("view3d.snap_selected_to_cursor", R"({"offset":true})")) &&
               near(fixture.worldPosition(fixture.front), {1.0f, 2.0f, 5.5f}) &&
               near(fixture.worldPosition(fixture.behind), {1.0f, 2.0f, 0.5f}),
           "with offset the selection's median should land on the cursor");
    fixture.selection.clear();
    expect(fixture.invoke("view3d.snap_selected_to_cursor").status == OperatorStatus::Cancelled &&
               fixture.invoke("view3d.snap_cursor_to_selected").status == OperatorStatus::Cancelled,
           "the selection snaps need a selection");
}

void testSpawnAtCursor() {
    Fixture fixture("spawn");
    Frames frames;
    frames.settle(fixture);
    fixture.cursor() = glm::vec3(1.0f, 2.0f, 3.0f);

    expect(static_cast<bool>(fixture.invoke("object.add_empty")) && near(fixture.activePosition(), {1.0f, 2.0f, 3.0f}),
           "object.add_empty should spawn at the 3D cursor");
    const LastOperator* last = fixture.runner.lastOperator();
    expect(last != nullptr && near(last->properties.get<AddObjectProperties>()->location, {1.0f, 2.0f, 3.0f}),
           "Adjust Last Operation should show the cursor as the location");
    if (last != nullptr) {
        OperatorProperties adjusted = last->properties;
        adjusted.get<AddObjectProperties>()->location = glm::vec3(4.0f, 0.0f, 0.0f);
        OperatorContext context{.editing = fixture.editing(), .selection = fixture.selection, .app = &fixture.app};
        expect(static_cast<bool>(fixture.runner.adjustLast(context, adjusted)) &&
                   near(fixture.activePosition(), {4.0f, 0.0f, 0.0f}),
               "an adjusted location should win over the cursor");
    }
    expect(static_cast<bool>(fixture.invoke("object.add_empty", R"({"location":[0,1,0]})")) &&
               near(fixture.activePosition(), {0.0f, 1.0f, 0.0f}),
           "a location the caller passes should win over the cursor");
    expect(static_cast<bool>(fixture.invoke("object.add_cube")) && near(fixture.activePosition(), {1.0f, 2.0f, 3.0f}),
           "object.add_cube should spawn at the cursor");
    expect(static_cast<bool>(fixture.invoke("object.add_empty_with", R"({"component":"point_light"})")) &&
               near(fixture.activePosition(), {1.0f, 2.0f, 3.0f}),
           "object.add_empty_with should spawn at the cursor");

    // A dropped model lands at the cursor as one undo step.
    EditorContext context = fixture.context();
    const size_t before = fixture.scene.registry().view<const SceneEntityIdComponent>().size();
    context.placeModel(fixture.root / defaultCubeModel, entt::null);
    expect(fixture.scene.registry().view<const SceneEntityIdComponent>().size() > before &&
               near(fixture.activePosition(), {1.0f, 2.0f, 3.0f}),
           "a dropped model should spawn at the cursor");
    fixture.editing().undo();
    expect(fixture.scene.registry().view<const SceneEntityIdComponent>().size() == before,
           "undo should remove the dropped model");

    // Prefab instances, from a drop or the Add menu, land at the cursor.
    const Entity lamp = fixture.scene.createEntity("Lamp");
    fixture.selection.selectOnly(fixture.scene.id(lamp));
    expect(static_cast<bool>(fixture.invoke("prefab.create_from_selection", R"({"path":"lamp.prefab"})")),
           "the test prefab should be created");
    fixture.cursor() = glm::vec3(-2.0f, 0.5f, 1.0f);
    context.placePrefab(fixture.root / "lamp.prefab", entt::null);
    expect(near(fixture.activePosition(), {-2.0f, 0.5f, 1.0f}) && fixture.selection.active() != fixture.scene.id(lamp),
           "a dropped prefab should be instantiated at the cursor");
    expect(static_cast<bool>(fixture.invoke("prefab.instantiate", R"({"asset":"lamp.prefab","location":[0,0,9]})")) &&
               near(fixture.activePosition(), {0.0f, 0.0f, 9.0f}),
           "an explicit prefab location should win over the cursor");
}

// --- Tool settings, area state, drawing -----------------------------------

void testToolOperatorsAndState() {
    Fixture fixture("tools");
    Frames frames;
    frames.settle(fixture);
    expect(static_cast<bool>(fixture.invoke("transform.pivot_set", R"({"pivot":"cursor"})")) &&
               fixture.panel->tools().pivot == TransformPivot::Cursor,
           "transform.pivot_set should set the active viewport's pivot point");
    expect(static_cast<bool>(fixture.invoke("transform.snap_target_set", R"({"target":"vertex"})")) &&
               fixture.panel->tools().snapTarget == SnapTarget::Vertex,
           "transform.snap_target_set should set the active viewport's snap target");
    expect(!fixture.invoke("transform.pivot_set", R"({"pivot":"nowhere"})"), "an unknown pivot should be refused");

    TransformToolSettings& tools = fixture.panel->tools();
    tools.snapping = true;
    tools.absoluteGridSnap = true;
    tools.translationSnap = 0.25f;
    tools.pivot = TransformPivot::IndividualOrigins;
    fixture.panel->chrome().overlays.cursor = false;
    fixture.panel->chrome().overlays.grid = false;
    const std::string state = fixture.panel->areaState();
    ViewportRegistry otherRegistry;
    ViewportPanel restored(otherRegistry);
    restored.restoreAreaState(state);
    expect(restored.tools().snapping && restored.tools().absoluteGridSnap &&
               near(restored.tools().translationSnap, 0.25f) &&
               restored.tools().pivot == TransformPivot::IndividualOrigins &&
               restored.tools().snapTarget == SnapTarget::Vertex && !restored.chrome().overlays.cursor &&
               !restored.chrome().overlays.grid,
           "the area state should keep the tool choices beside the overlays");
    ViewportPanel defaults(otherRegistry);
    defaults.restoreAreaState(R"({"overlays":{"grid":false},"tools":{"pivot":7,"translation_snap":-1}})");
    expect(defaults.tools().pivot == TransformPivot::MedianPoint && near(defaults.tools().translationSnap, 1.0f) &&
               !defaults.chrome().overlays.grid,
           "invalid tool values should keep their defaults");
    expect(static_cast<bool>(fixture.invoke("view3d.overlay_toggle", R"({"overlay":"cursor_3d"})")) &&
               fixture.panel->chrome().overlays.cursor,
           "view3d.overlay_toggle should show the 3D cursor again");
}

void testCursorDrawing() {
    const RenderView lens{.position = {0.0f, 0.0f, 10.0f},
                          .forward = {0.0f, 0.0f, -1.0f},
                          .up = {0.0f, 1.0f, 0.0f},
                          .verticalFovRadians = glm::radians(60.0f),
                          .nearPlane = 0.1f,
                          .farPlane = 1000.0f};
    const glm::vec2 viewSize(800.0f, 600.0f);
    DebugDraw output;
    recordCursor3D(glm::vec3(0.0f), lens, viewSize, output);
    expect(output.lineCount() == 20 && output.lineVertices(DebugDepth::Tested).empty(),
           "the cursor should draw a dashed ring and four ticks over every surface");
    // The ring stays the same size on screen at any distance.
    const auto ringPixels = [&](const glm::vec3& location) {
        DebugDraw ring;
        recordCursor3D(location, lens, viewSize, ring);
        const glm::mat4 viewProjection = lensProjectionMatrix(lens, viewSize.x / viewSize.y) * lensViewMatrix(lens);
        const auto toPixels = [&](const glm::vec3& point) {
            const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
            return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * viewSize;
        };
        return glm::length(toPixels(ring.lineVertices(DebugDepth::AlwaysOnTop).front().position) - toPixels(location));
    };
    expect(near(ringPixels(glm::vec3(0.0f)), cursorRingRadiusPixels, 0.05f) &&
               near(ringPixels({0.0f, 0.0f, -40.0f}), cursorRingRadiusPixels, 0.05f),
           "the cursor ring should keep its size on screen");
    DebugDraw behindLens;
    recordCursor3D({0.0f, 0.0f, 20.0f}, lens, viewSize, behindLens);
    expect(behindLens.empty(), "a cursor behind the lens should draw nothing");

    // Each viewport records it into its own view's list.
    Fixture fixture("drawing");
    Frames frames;
    frames.settle(fixture);
    const auto recordedLines = [&]() -> size_t {
        DebugDraw* list = fixture.rendering.viewDebugDraw(fixture.panel->renderView());
        if (list == nullptr) {
            return 0;
        }
        list->clear();
        frames.frame(fixture, {2.0f, 700.0f});
        return list->lineVertices(DebugDepth::AlwaysOnTop).size() / 2;
    };
    const size_t withCursor = recordedLines();
    fixture.panel->chrome().overlays.cursor = false;
    const size_t withoutCursor = recordedLines();
    expect(fixture.panel->renderView() && withCursor == withoutCursor + 20,
           "the viewport should draw the cursor unless its overlay is off");
}

// --- Persistence ----------------------------------------------------------

void testCursorPersistence() {
    const std::filesystem::path root = freshDirectory("persistence");
    const std::filesystem::path scenePath = root / "level.zenith-scene";
    const std::filesystem::path sidecar = sceneEditorDataPath(scenePath);
    expect(sidecar.filename() == "level.zenith-scene.editor.json", "the sidecar should sit beside the scene");
    AssetManager assets;
    Scene scene;
    scene.createEntity("Thing");
    {
        EditorDocument document(root);
        expect(document.saveAs(scenePath, scene, assets).success && !std::filesystem::exists(sidecar),
               "a scene with the default cursor should get no sidecar");
        document.editorData().cursorLocation = glm::vec3(1.5f, -2.0f, 3.25f);
        expect(document.dirty(), "a moved cursor should be an unsaved change");
        expect(document.save(scene, assets).success && std::filesystem::exists(sidecar) && !document.dirty(),
               "saving should write the moved cursor beside the scene");
    }
    {
        EditorDocument document(root);
        Scene loaded;
        expect(document.open(scenePath, loaded, assets).success &&
                   near(document.editorData().cursorLocation, {1.5f, -2.0f, 3.25f}) &&
                   document.takeEditorDataWarning().empty(),
               "opening the scene should restore its cursor");
        document.createNew(loaded);
        expect(near(document.editorData().cursorLocation, glm::vec3(0.0f)), "a new scene should start at the origin");
    }
    {
        EditorDocument document(root);
        Scene loaded;
        expect(document.open(scenePath, loaded, assets).success, "the scene should open again");
        document.editorData().cursorLocation = glm::vec3(0.0f);
        expect(document.save(loaded, assets).success && !std::filesystem::exists(sidecar),
               "saving the default cursor should remove the sidecar");
    }
    {
        std::ofstream broken(sidecar);
        broken << R"({"version":1,"cursor":{"location":[1,"two",3]}})";
    }
    {
        EditorDocument document(root);
        Scene loaded;
        expect(document.open(scenePath, loaded, assets).success &&
                   near(document.editorData().cursorLocation, glm::vec3(0.0f)),
               "a malformed sidecar should not keep the scene from opening");
        expect(!document.takeEditorDataWarning().empty() && document.takeEditorDataWarning().empty(),
               "a malformed sidecar should leave one warning");
    }
    {
        std::ofstream later(sidecar);
        later << R"({"version":2,"cursor":{"location":[4,5,6],"rotation":[0,0,0,1]},"future":true})";
    }
    SceneEditorData data;
    expect(readSceneEditorData(scenePath, data).success && near(data.cursorLocation, {4.0f, 5.0f, 6.0f}),
           "members a later version adds should be ignored");
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
} // namespace

int main() {
    testPivotModes();
    testIncrementSnapping();
    testTargetSnapping();
    testSnapQueries();
    testCursorPlacement();
    testSpawnAtCursor();
    testToolOperatorsAndState();
    testCursorDrawing();
    testCursorPersistence();
    if (failures == 0) {
        std::cout << "Snapping and cursor tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
