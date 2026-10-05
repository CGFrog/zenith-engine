#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/EntityPropertiesView.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// The editor's collider tools: switching a collider's shape in the inspector
// (keeping its size, as one undo step), the meshes a triangle-mesh collider
// may choose from.

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(float actual, float expected, float tolerance = 1e-4f) {
    return std::abs(actual - expected) <= tolerance;
}

constexpr const char* propertiesWindow = "Collider test";

struct Fixture : EditorTestFixture {
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
    expect(window != nullptr, "the properties window should be drawn");
    if (window == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "collider", "Shape"));
    fixture.drawProperties();
    fixture.drawProperties();
    const ImGuiWindow* popup = HeadlessImGuiContext::findWindow("##Combo_");
    expect(popup != nullptr && popup->Active, "the Shape combo should open");
    if (popup == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*popup, label));
    fixture.drawProperties();
    fixture.drawProperties();
}

void testShapeConversion() {
    using ComponentEditors::ColliderShapeKind;
    using ComponentEditors::convertColliderShape;
    const ColliderShape box = BoxCollider{.halfExtents = {0.5f, 1.0f, 0.25f}};
    const ColliderShape asSphere = convertColliderShape(box, ColliderShapeKind::Sphere, {});
    expect(std::holds_alternative<SphereCollider>(asSphere) && near(std::get<SphereCollider>(asSphere).radius, 1.0f),
           "a box becomes a sphere as large as its largest half extent");
    const ColliderShape asCapsule = convertColliderShape(box, ColliderShapeKind::Capsule, {});
    const auto& capsule = std::get<CapsuleCollider>(asCapsule);
    expect(near(capsule.radius, 0.5f) && near(capsule.halfHeight + capsule.radius, 1.0f),
           "a box becomes a capsule as tall as it, as wide as its wider side");
    const ColliderShape backToBox = convertColliderShape(asCapsule, ColliderShapeKind::Box, {});
    expect(std::get<BoxCollider>(backToBox).halfExtents == glm::vec3(0.5f, 1.0f, 0.5f),
           "a capsule becomes the box around it");
    const ColliderShape flat =
        convertColliderShape(BoxCollider{.halfExtents = {2.0f, 0.1f, 2.0f}}, ColliderShapeKind::Capsule, {});
    expect(std::get<CapsuleCollider>(flat).halfHeight > 0.0f, "a capsule from a flat box keeps a positive height");
    const MeshHandle mesh{.index = 3, .generation = 1};
    expect(std::get<TriangleMeshCollider>(convertColliderShape(box, ColliderShapeKind::TriangleMesh, mesh)).mesh ==
               mesh,
           "a triangle mesh takes the chosen mesh");
}

void testShapeSwitchInInspector() {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    fixture.drawProperties();
    chooseShape(fixture, "Sphere");
    const auto* sphere = std::get_if<SphereCollider>(&fixture.collider().shape);
    expect(sphere != nullptr && near(sphere->radius, 1.0f), "choosing Sphere turns the box into a sphere its size");
    expect(fixture.history.size() == 1, "switching the shape is one undo step");
    fixture.editing().undo();
    expect(std::holds_alternative<BoxCollider>(fixture.collider().shape) &&
               std::get<BoxCollider>(fixture.collider().shape).halfExtents == glm::vec3(0.5f, 1.0f, 0.25f),
           "undo restores the box");

    // Without a model mesh, Triangle Mesh cannot be chosen.
    chooseShape(fixture, "Triangle Mesh");
    expect(std::holds_alternative<BoxCollider>(fixture.collider().shape),
           "a collider without a mesh to use cannot become a triangle mesh");
}

void testMeshChoices() {
    Fixture fixture;
    const AssetLoadResult<MeshHandle> cube = fixture.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
    expect(static_cast<bool>(cube), "the cube mesh should load: " + cube.error);
    if (!cube) {
        return;
    }
    const Entity entity = fixture.scene.findEntity(fixture.entity);
    expect(ComponentEditors::colliderMeshChoices(fixture.scene, fixture.assets, entity).empty(),
           "an entity without meshes offers none");
    fixture.scene.registry().emplace<MeshRendererComponent>(entity, MeshRendererComponent{.mesh = cube.handle});
    const std::vector<ComponentEditors::ColliderMeshChoice> choices =
        ComponentEditors::colliderMeshChoices(fixture.scene, fixture.assets, entity);
    expect(choices.size() == 1 && choices.front().mesh == cube.handle && !choices.front().name.empty(),
           "an entity's own mesh is offered");

    HeadlessImGuiContext imgui;
    fixture.drawProperties();
    chooseShape(fixture, "Triangle Mesh");
    const auto* triangles = std::get_if<TriangleMeshCollider>(&fixture.collider().shape);
    expect(triangles != nullptr && triangles->mesh == cube.handle, "choosing Triangle Mesh uses the entity's mesh");
}
} // namespace

int main(int, char** argv) {
    initializeResourceLocator(argv[0]);
    testShapeConversion();
    testShapeSwitchInInspector();
    testMeshChoices();
    if (failures != 0) {
        std::cerr << failures << " collider tool test(s) failed\n";
        return 1;
    }
    return 0;
}
