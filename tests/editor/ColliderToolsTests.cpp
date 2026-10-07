#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/viewport/ColliderHandles.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// The editor's collider tools: switching a collider's shape in the inspector
// (keeping its size, as one undo step), the meshes a triangle-mesh collider
// may choose from, and the viewport handles (placement, drag maths, and a
// drag as one undo step or cancelled).

TEST_SUITE_BEGIN("ColliderTools");

namespace {

bool near(float actual, float expected, float tolerance = 1e-4f) {
    return std::abs(actual - expected) <= tolerance;
}

constexpr const char* propertiesWindow = "Collider test";

// Points the resource locator at the build tree's assets, as the editor does
// from its executable path. A base of Fixture, so it runs before the editor
// fixture reads assetRoot().
struct LocatedResources {
    LocatedResources() {
        initializeResourceLocator(std::filesystem::path(ZENITH_ASSET_DIR).parent_path() / "zenith_editor_tests");
    }
};

struct Fixture : LocatedResources, EditorTestFixture {
    EntityPropertiesView view{componentLayout};
    SceneEntityId entity;

    // Documents resolve the engine's assets, so a mesh reference saves.
    Fixture()
        : EditorTestFixture({.documentRoot = assetRoot()}) {
        const Entity created = scene.createEntity("Crate");
        scene.registry().emplace<ColliderComponent>(
            created, ColliderComponent{.shape = BoxCollider{.halfExtents = {0.5f, 1.0f, 0.25f}}});
        entity = scene.id(created);
        selection.selectOnly(entity);
    }

    ColliderComponent& collider() {
        return scene.registry().get<ColliderComponent>(scene.findEntity(entity));
    }

    void drawProperties() {
        HeadlessImGuiFrame frame(propertiesWindow);
        EditorContext editor = context();
        view.drawComponent(editor, *componentTypesFor(document.options()).find("collider"), std::span(&entity, 1), {});
    }
};

// Picks label from the inspector's "Shape" combo over three frames: open it,
// let the popup appear, then press the item.
void chooseShape(Fixture& fixture, const char* label) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow(propertiesWindow);
    CHECK_MESSAGE(window != nullptr, "the properties window should be drawn");
    if (window == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "collider", "Shape"));
    fixture.drawProperties();
    fixture.drawProperties();
    const ImGuiWindow* popup = HeadlessImGuiContext::findWindow("##Combo_");
    CHECK_MESSAGE((popup != nullptr && popup->Active), "the Shape combo should open");
    if (popup == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*popup, label));
    fixture.drawProperties();
    fixture.drawProperties();
}

TEST_CASE("ColliderTools: shape conversion") {
    using ComponentEditors::ColliderShapeKind;
    using ComponentEditors::convertColliderShape;
    const ColliderShape box = BoxCollider{.halfExtents = {0.5f, 1.0f, 0.25f}};
    const ColliderShape asSphere = convertColliderShape(box, ColliderShapeKind::Sphere, {});
    CHECK_MESSAGE(
        (std::holds_alternative<SphereCollider>(asSphere) && near(std::get<SphereCollider>(asSphere).radius, 1.0f)),
        "a box becomes a sphere as large as its largest half extent");
    const ColliderShape asCapsule = convertColliderShape(box, ColliderShapeKind::Capsule, {});
    const auto& capsule = std::get<CapsuleCollider>(asCapsule);
    CHECK_MESSAGE((near(capsule.radius, 0.5f) && near(capsule.halfHeight + capsule.radius, 1.0f)),
                  "a box becomes a capsule as tall as it, as wide as its wider side");
    const ColliderShape backToBox = convertColliderShape(asCapsule, ColliderShapeKind::Box, {});
    CHECK_MESSAGE(std::get<BoxCollider>(backToBox).halfExtents == glm::vec3(0.5f, 1.0f, 0.5f),
                  "a capsule becomes the box around it");
    const ColliderShape flat =
        convertColliderShape(BoxCollider{.halfExtents = {2.0f, 0.1f, 2.0f}}, ColliderShapeKind::Capsule, {});
    CHECK_MESSAGE(std::get<CapsuleCollider>(flat).halfHeight > 0.0f,
                  "a capsule from a flat box keeps a positive height");
    const MeshHandle mesh{.index = 3, .generation = 1};
    CHECK_MESSAGE(
        std::get<TriangleMeshCollider>(convertColliderShape(box, ColliderShapeKind::TriangleMesh, mesh)).mesh == mesh,
        "a triangle mesh takes the chosen mesh");
}

TEST_CASE("ColliderTools: shape switch in inspector") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    fixture.drawProperties();
    chooseShape(fixture, "Sphere");
    const auto* sphere = std::get_if<SphereCollider>(&fixture.collider().shape);
    CHECK_MESSAGE((sphere != nullptr && near(sphere->radius, 1.0f)),
                  "choosing Sphere turns the box into a sphere its size");
    CHECK_MESSAGE(fixture.history.size() == 1, "switching the shape is one undo step");
    fixture.editing().undo();
    CHECK_MESSAGE((std::holds_alternative<BoxCollider>(fixture.collider().shape) &&
                   std::get<BoxCollider>(fixture.collider().shape).halfExtents == glm::vec3(0.5f, 1.0f, 0.25f)),
                  "undo restores the box");

    // Without a model mesh, Triangle Mesh cannot be chosen.
    chooseShape(fixture, "Triangle Mesh");
    CHECK_MESSAGE(std::holds_alternative<BoxCollider>(fixture.collider().shape),
                  "a collider without a mesh to use cannot become a triangle mesh");
}

TEST_CASE("ColliderTools: mesh choices") {
    Fixture fixture;
    const AssetLoadResult<MeshHandle> cube = fixture.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
    CHECK_MESSAGE(static_cast<bool>(cube), ("the cube mesh should load: " + cube.error));
    if (!cube) {
        return;
    }
    const Entity entity = fixture.scene.findEntity(fixture.entity);
    CHECK_MESSAGE(ComponentEditors::colliderMeshChoices(fixture.scene, fixture.assets, entity).empty(),
                  "an entity without meshes offers none");
    fixture.scene.registry().emplace<MeshRendererComponent>(entity, MeshRendererComponent{.mesh = cube.handle});
    const std::vector<ComponentEditors::ColliderMeshChoice> choices =
        ComponentEditors::colliderMeshChoices(fixture.scene, fixture.assets, entity);
    CHECK_MESSAGE((choices.size() == 1 && choices.front().mesh == cube.handle && !choices.front().name.empty()),
                  "an entity's own mesh is offered");

    HeadlessImGuiContext imgui;
    fixture.drawProperties();
    chooseShape(fixture, "Triangle Mesh");
    const auto* triangles = std::get_if<TriangleMeshCollider>(&fixture.collider().shape);
    CHECK_MESSAGE((triangles != nullptr && triangles->mesh == cube.handle),
                  "choosing Triangle Mesh uses the entity's mesh");
}

bool nearVector(const glm::vec3& actual, const glm::vec3& expected, float tolerance = 1e-4f) {
    return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::vec3(tolerance)));
}

const ColliderHandle* findHandle(const std::vector<ColliderHandle>& handles, ColliderHandle::Dimension dimension,
                                 int axis, float side) {
    for (const ColliderHandle& handle : handles) {
        if (handle.dimension == dimension && handle.axis == axis && handle.side == side) {
            return &handle;
        }
    }
    return nullptr;
}

TEST_CASE("ColliderTools: handle placement") {
    const glm::mat4 world =
        glm::translate(glm::mat4(1.0f), {1.0f, 0.0f, 0.0f}) * glm::scale(glm::mat4(1.0f), {2.0f, 1.0f, 1.0f});
    const ColliderComponent box{.shape = BoxCollider{.halfExtents = {0.5f, 0.25f, 1.0f}},
                                .offsetPosition = {0.0f, 1.0f, 0.0f}};
    const std::vector<ColliderHandle> boxHandles = colliderHandles(box, world);
    const ColliderHandle* right = findHandle(boxHandles, ColliderHandle::Dimension::BoxHalfExtent, 0, 1.0f);
    CHECK_MESSAGE((boxHandles.size() == 6 && right != nullptr && nearVector(right->position, {2.0f, 1.0f, 0.0f}) &&
                   nearVector(right->direction, {1.0f, 0.0f, 0.0f})),
                  "a box has a handle at each face centre, scaled and offset like its body");

    const ColliderComponent capsule{.shape = CapsuleCollider{.halfHeight = 0.5f, .radius = 0.25f}};
    const std::vector<ColliderHandle> capsuleHandles = colliderHandles(capsule, glm::mat4(1.0f));
    const ColliderHandle* top = findHandle(capsuleHandles, ColliderHandle::Dimension::CapsuleHalfHeight, 1, 1.0f);
    CHECK_MESSAGE((capsuleHandles.size() == 6 && top != nullptr && nearVector(top->position, {0.0f, 0.75f, 0.0f})),
                  "a capsule has four radius handles and one at each end's tip");
    CHECK_MESSAGE(colliderHandles(ColliderComponent{.shape = TriangleMeshCollider{}}, glm::mat4(1.0f)).empty(),
                  "a triangle mesh collider has no handles");
}

TEST_CASE("ColliderTools: handle drag maths") {
    const ColliderComponent box{.shape = BoxCollider{.halfExtents = {0.5f, 0.5f, 0.5f}}};
    const glm::vec3 scale{2.0f, 1.0f, 1.0f};
    const glm::mat4 world = glm::scale(glm::mat4(1.0f), scale);
    const ColliderHandle right =
        *findHandle(colliderHandles(box, world), ColliderHandle::Dimension::BoxHalfExtent, 0, 1.0f);
    const ColliderComponent symmetric = dragColliderHandle(box, right, scale, 1.0f, ColliderHandleDrag::Symmetric);
    CHECK_MESSAGE((near(std::get<BoxCollider>(symmetric.shape).halfExtents.x, 1.0f) &&
                   symmetric.offsetPosition == glm::vec3(0.0f)),
                  "a symmetric drag grows both sides by the distance and keeps the shape centred");

    const ColliderComponent oneSided = dragColliderHandle(box, right, scale, 1.0f, ColliderHandleDrag::OneSided);
    const float halfX = std::get<BoxCollider>(oneSided.shape).halfExtents.x;
    const float centre = oneSided.offsetPosition.x * scale.x;
    CHECK_MESSAGE((near(halfX, 0.75f) && near(centre + halfX * scale.x, 2.0f) && near(centre - halfX * scale.x, -1.0f)),
                  "a one-sided drag moves only the dragged face, shifting the offset");

    const ColliderHandle left =
        *findHandle(colliderHandles(box, world), ColliderHandle::Dimension::BoxHalfExtent, 0, -1.0f);
    const ColliderComponent collapsed = dragColliderHandle(box, left, scale, -10.0f, ColliderHandleDrag::OneSided);
    const float collapsedHalf = std::get<BoxCollider>(collapsed.shape).halfExtents.x;
    CHECK_MESSAGE((near(collapsedHalf, 0.001f) &&
                   near(collapsed.offsetPosition.x * scale.x + collapsedHalf * scale.x, 1.0f, 1e-3f)),
                  "dragging a face past the other stops at the minimum size with the other face in place");

    const ColliderComponent turned{.shape = SphereCollider{.radius = 0.5f},
                                   .offsetRotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1))};
    const ColliderHandle up =
        *findHandle(colliderHandles(turned, glm::mat4(1.0f)), ColliderHandle::Dimension::Radius, 0, 1.0f);
    const ColliderComponent movedSphere =
        dragColliderHandle(turned, up, glm::vec3(1.0f), 1.0f, ColliderHandleDrag::OneSided);
    CHECK_MESSAGE((near(std::get<SphereCollider>(movedSphere.shape).radius, 1.0f) &&
                   nearVector(movedSphere.offsetPosition, {0.0f, 0.5f, 0.0f})),
                  "a turned shape's one-sided drag follows the handle's turned axis");
}

// The lens looks down -Z at the origin from 10 m; a 200 by 100 pixel view.
RenderView frontLens() {
    return {.position = {0.0f, 0.0f, 10.0f}, .forward = {0.0f, 0.0f, -1.0f}, .up = {0.0f, 1.0f, 0.0f}};
}

constexpr glm::vec2 viewSize{200.0f, 100.0f};

glm::vec2 viewPixel(const glm::vec3& point) {
    const RenderView lens = frontLens();
    const glm::vec4 clip =
        lensProjectionMatrix(lens, viewSize.x / viewSize.y) * lensViewMatrix(lens) * glm::vec4(point, 1.0f);
    return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * viewSize;
}

TEST_CASE("ColliderTools: axis parameter") {
    const RenderView lens = frontLens();
    const std::optional<float> atOrigin =
        colliderHandleAxisParameter(lens, viewSize, viewPixel({0.0f, 0.0f, 0.0f}), {0.0f, 0.0f, 0.0f}, {1, 0, 0});
    const std::optional<float> atTwo =
        colliderHandleAxisParameter(lens, viewSize, viewPixel({2.0f, 0.0f, 0.0f}), {0.0f, 0.0f, 0.0f}, {1, 0, 0});
    CHECK_MESSAGE((atOrigin && near(*atOrigin, 0.0f, 1e-3f) && atTwo && near(*atTwo, 2.0f, 1e-3f)),
                  "the axis parameter is where the mouse ray passes the handle's line");
    CHECK_MESSAGE(!colliderHandleAxisParameter(lens, viewSize, viewSize * 0.5f, {0.0f, 0.0f, 0.0f}, {0, 0, 1}),
                  "a line pointing at the lens cannot be dragged");
}

// Drives the tool over frames with the mouse in view pixels.
struct HandleDriver {
    Fixture& fixture;
    ColliderHandleTool tool;

    bool frame(glm::vec2 mouse, bool pressed, bool down, bool ctrl = false, bool cancel = false) {
        EditorContext editor = fixture.context();
        return tool.update(editor, frontLens(), viewSize,
                           {.mouse = mouse,
                            .available = true,
                            .leftPressed = pressed,
                            .leftDown = down,
                            .cancel = cancel,
                            .ctrl = ctrl},
                           nullptr, glm::vec2(0.0f));
    }
};

TEST_CASE("ColliderTools: handle drag undo") {
    Fixture fixture;
    HandleDriver driver{fixture};
    // The box's +X face handle at (0.5, 0, 0).
    const glm::vec2 handle = viewPixel({0.5f, 0.0f, 0.0f});
    CHECK_MESSAGE(!driver.frame(handle + glm::vec2(0.0f, 30.0f), false, false),
                  "away from every handle the viewport picks as usual");
    CHECK_MESSAGE((driver.frame(handle, false, false) && driver.tool.hovered().has_value()),
                  "a hovered handle blocks picking");
    driver.frame(handle, true, true);
    CHECK_MESSAGE(driver.tool.dragging(), "pressing a handle starts a drag");
    driver.frame(viewPixel({1.5f, 0.0f, 0.0f}), false, true);
    driver.frame(viewPixel({1.5f, 0.0f, 0.0f}), false, false);
    CHECK_MESSAGE((near(std::get<BoxCollider>(fixture.collider().shape).halfExtents.x, 1.5f, 1e-3f) &&
                   fixture.history.size() == 1 && !driver.tool.dragging()),
                  "dragging the face a metre out grows both sides and is one undo step");
    fixture.editing().undo();
    CHECK_MESSAGE(near(std::get<BoxCollider>(fixture.collider().shape).halfExtents.x, 0.5f),
                  "undo restores the dragged collider");

    driver.frame(handle, true, true);
    driver.frame(viewPixel({1.5f, 0.0f, 0.0f}), false, true, true);
    CHECK_MESSAGE(fixture.collider().offsetPosition.x > 0.4f, "Ctrl drags one side, moving the offset");
    driver.frame(viewPixel({1.5f, 0.0f, 0.0f}), false, true, true, true);
    CHECK_MESSAGE((near(std::get<BoxCollider>(fixture.collider().shape).halfExtents.x, 0.5f) &&
                   fixture.collider().offsetPosition == glm::vec3(0.0f) && fixture.history.size() == 1 &&
                   !driver.tool.dragging()),
                  "a cancelled drag restores the collider and records nothing");
}
} // namespace

TEST_SUITE_END();
