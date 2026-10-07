#include "editor/application/EditorApplication.hpp"
#include "editor/interaction/EditorCamera.hpp"

#include "support/Doctest.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("EditorCamera");

namespace {

bool near(glm::vec3 left, glm::vec3 right, float tolerance = 0.0001f) {
    return glm::length(left - right) <= tolerance;
}

// The sample model at the origin and a camera focused on it, for the picking
// cases. The model is loaded but not yet instantiated.
struct PickingScene {
    PickingScene()
        : model(assets.loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf")) {
        camera.focus({0.0f, 0.0f, 0.0f}, 2.0f);
    }

    bool instantiate() {
        const bool instantiated = scene.instantiateModel(root, model.handle, assets).success;
        scene.updateWorldTransforms();
        return instantiated;
    }

    AssetManager assets;
    AssetLoadResult<ModelHandle> model;
    Scene scene;
    Entity root{scene.createEntity("Model")};
    EditorCamera camera;
};

} // namespace

TEST_CASE("EditorCamera: the editor captures the cursor only during right-button freelook") {
    EditorApplication editor({}, {}, {});
    CHECK_MESSAGE(editor.mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
                  "the editor should only capture the cursor during right-button freelook");
}

TEST_CASE("EditorCamera: dragging right rotates the camera toward the right") {
    EditorCamera horizontalRotation;
    InputState rotateRight;
    rotateRight.setMouseButtonDown(MouseButton::Right);
    rotateRight.mouseDelta.x = 40.0f;
    horizontalRotation.update(rotateRight, 0.0f, true);
    CHECK_MESSAGE(horizontalRotation.renderView().forward.x > 0.0f,
                  "dragging right should rotate the editor camera toward the right");
}

TEST_CASE("EditorCamera: speed clamps and focus points the camera at a position") {
    EditorCamera camera;
    camera.setSpeed(-1.0f);
    CHECK_MESSAGE(camera.speed() == 0.05f, "camera speed should clamp to its positive minimum");
    camera.setSpeed(2000.0f);
    CHECK_MESSAGE(camera.speed() == 1000.0f, "camera speed should clamp to its maximum");
    camera.focus({0.0f, 0.0f, 0.0f}, 2.0f);
    const RenderView view = camera.renderView();
    CHECK_MESSAGE(glm::dot(glm::normalize(-view.position), view.forward) > 0.999f,
                  "focus should point the camera at the requested world position");
}

TEST_CASE("EditorCamera: mouse buttons and keys navigate as documented") {
    // The camera as the speed and focus case leaves it.
    EditorCamera camera;
    camera.setSpeed(2000.0f);
    camera.focus({0.0f, 0.0f, 0.0f}, 2.0f);
    const RenderView view = camera.renderView();

    InputState select;
    select.setMouseButtonDown(MouseButton::Left);
    select.mouseDelta = {80.0f, 40.0f};
    camera.update(select, 1.0f / 60.0f, true);
    CHECK_MESSAGE(
        (near(camera.renderView().position, view.position) && near(camera.renderView().forward, view.forward)),
        "left dragging should not navigate the editor camera");

    InputState orbit;
    orbit.setMouseButtonDown(MouseButton::Middle);
    orbit.mouseDelta = {80.0f, 40.0f};
    camera.update(orbit, 1.0f / 60.0f, true);
    CHECK_MESSAGE(!near(camera.renderView().forward, view.forward), "middle dragging should orbit around the pivot");

    const RenderView beforePan = camera.renderView();
    InputState pan;
    pan.setMouseButtonDown(MouseButton::Middle);
    pan.setKeyDown(Key::LeftShift);
    pan.mouseDelta = {80.0f, 40.0f};
    camera.update(pan, 1.0f / 60.0f, true);
    CHECK_MESSAGE((!near(camera.renderView().position, beforePan.position) &&
                   near(camera.renderView().forward, beforePan.forward)),
                  "Shift+middle dragging should pan without rotating");

    const glm::vec3 beforeZoom = camera.renderView().position;
    InputState zoom;
    zoom.mouseWheelDelta.y = 1.0f;
    camera.update(zoom, 1.0f / 60.0f, true);
    CHECK_MESSAGE(!near(camera.renderView().position, beforeZoom), "mouse wheel should zoom toward the orbit pivot");

    const RenderView beforeInactiveWasd = camera.renderView();
    InputState inactiveWasd;
    inactiveWasd.setKeyDown(Key::W);
    camera.update(inactiveWasd, 1.0f, true);
    CHECK_MESSAGE(near(camera.renderView().position, beforeInactiveWasd.position),
                  "WASD should not move the editor camera outside right-button freelook");

    InputState freelook;
    freelook.setMouseButtonDown(MouseButton::Right);
    freelook.setKeyDown(Key::W);
    freelook.mouseDelta = {40.0f, 10.0f};
    camera.update(freelook, 1.0f, true);
    CHECK_MESSAGE((!near(camera.renderView().position, beforeInactiveWasd.position) &&
                   !near(camera.renderView().forward, beforeInactiveWasd.forward)),
                  "right-button freelook should rotate the camera and enable WASD movement");
}

TEST_CASE("EditorCamera: rendering and gizmos use their own vertical projection conventions") {
    EditorCamera camera;
    CHECK_MESSAGE((camera.projectionMatrix(1.0f)[1][1] < 0.0f && camera.gizmoProjectionMatrix(1.0f)[1][1] > 0.0f),
                  "rendering and screen-space gizmos should use their respective vertical projection conventions");
}

// The lens fields feed the render view and both projections alike.
TEST_CASE("EditorCamera: the lens feeds the render view and both projections") {
    EditorCamera lens;
    CHECK_MESSAGE((std::abs(lens.verticalFovRadians() - glm::radians(60.0f)) < 0.0001f && lens.nearPlane() == 0.1f &&
                   lens.farPlane() == 1000.0f),
                  "the editor camera should keep the previous 60 degree, 0.1 to 1000 lens by default");
    const float defaultFocalScale = lens.projectionMatrix(1.0f)[0][0];
    lens.setVerticalFovRadians(glm::radians(30.0f));
    lens.setClipPlanes(0.5f, 250.0f);
    const RenderView lensView = lens.renderView();
    CHECK_MESSAGE((std::abs(lensView.verticalFovRadians - glm::radians(30.0f)) < 0.0001f &&
                   lensView.nearPlane == 0.5f && lensView.farPlane == 250.0f),
                  "the render view should carry the camera's field of view and clip planes");
    const glm::mat4 narrow = lens.projectionMatrix(1.0f);
    const float expectedFocalScale = 1.0f / std::tan(glm::radians(15.0f));
    CHECK_MESSAGE((std::abs(narrow[0][0] - expectedFocalScale) < 0.0001f && narrow[0][0] > defaultFocalScale &&
                   std::abs(lens.gizmoProjectionMatrix(1.0f)[1][1] - expectedFocalScale) < 0.0001f),
                  "a narrower field of view should magnify both the render and gizmo projections");
    const glm::vec4 nearClip = narrow * glm::vec4(0.0f, 0.0f, -0.5f, 1.0f);
    const glm::vec4 farClip = narrow * glm::vec4(0.0f, 0.0f, -250.0f, 1.0f);
    CHECK_MESSAGE((std::abs(nearClip.z / nearClip.w) < 0.0001f && std::abs(farClip.z / farClip.w - 1.0f) < 0.0001f),
                  "the render projection should map the clip planes to depth 0 and 1");
    lens.setVerticalFovRadians(10.0f);
    CHECK_MESSAGE(lens.verticalFovRadians() < glm::radians(180.0f), "the field of view should clamp below 180 degrees");
    lens.setVerticalFovRadians(-1.0f);
    CHECK_MESSAGE(lens.verticalFovRadians() > 0.0f, "the field of view should clamp above zero");
    lens.setClipPlanes(-1.0f, -5.0f);
    CHECK_MESSAGE((lens.nearPlane() > 0.0f && lens.farPlane() > lens.nearPlane()),
                  "invalid clip planes should clamp to a positive, non-empty depth range");
    lens.setClipPlanes(10.0f, 2.0f);
    CHECK_MESSAGE((lens.nearPlane() == 10.0f && lens.farPlane() > 10.0f),
                  "a far plane before the near plane should move past it");
}

TEST_CASE("EditorCamera: picking selects model geometry through the camera's field of view") {
    PickingScene picking;
    CHECK_MESSAGE(static_cast<bool>(picking.model), "camera picking test model should load");
    if (picking.model) {
        CHECK_MESSAGE(picking.instantiate(), "camera picking test model should instantiate");
        CHECK_MESSAGE((picking.camera.pick(picking.scene, picking.assets, {0.5f, 0.5f}, 1.0f) != entt::null),
                      "center viewport ray should select visible model geometry");

        // Picking follows the lens: a point near the image edge looks almost
        // straight ahead through a narrow lens and hits the model, while the
        // same point through a wide lens looks far past it.
        const glm::vec2 edge{0.5f, 0.1f};
        EditorCamera wideLens = picking.camera;
        wideLens.setVerticalFovRadians(glm::radians(120.0f));
        EditorCamera narrowLens = picking.camera;
        narrowLens.setVerticalFovRadians(glm::radians(5.0f));
        CHECK_MESSAGE((narrowLens.pick(picking.scene, picking.assets, edge, 1.0f) != entt::null &&
                       wideLens.pick(picking.scene, picking.assets, edge, 1.0f) == entt::null),
                      "picking should cast through the camera's field of view");
    }
}

// Picking skips hidden and unselectable entities and collections, and a click
// on a collection instance picks its instancer.
TEST_CASE("EditorCamera: picking visibility") {
    PickingScene picking;
    REQUIRE_MESSAGE(static_cast<bool>(picking.model), "camera picking test model should load");
    REQUIRE_MESSAGE(picking.instantiate(), "camera picking test model should instantiate");
    const EditorCamera& camera = picking.camera;
    Scene& scene = picking.scene;
    const AssetManager& assets = picking.assets;

    const glm::vec2 center{0.5f, 0.5f};
    const Entity picked = camera.pick(scene, assets, center, 1.0f);
    entt::registry& registry = scene.registry();
    registry.emplace<VisibilityComponent>(picked, VisibilityComponent{.hidden = true});
    CHECK_MESSAGE(camera.pick(scene, assets, center, 1.0f) != picked, "a hidden entity should not be picked");
    registry.replace<VisibilityComponent>(picked, VisibilityComponent{.selectable = false});
    CHECK_MESSAGE(camera.pick(scene, assets, center, 1.0f) != picked, "an unselectable entity should not be picked");
    registry.remove<VisibilityComponent>(picked);

    SceneCollections collections;
    SceneCollectionId source;
    collections.create("Source", {}, source);
    collections.setFlag(source, CollectionFlag::Selectable, false);
    scene.setCollections(collections);
    std::vector<Entity> meshes;
    for (const Entity entity : registry.view<const MeshRendererComponent>()) {
        meshes.push_back(entity);
    }
    for (const Entity entity : meshes) {
        linkToCollection(scene, entity, source);
    }
    CHECK_MESSAGE((camera.pick(scene, assets, center, 1.0f) == entt::null),
                  "members of an unselectable collection should not be picked");

    collections.setFlag(source, CollectionFlag::Selectable, true);
    collections.setFlag(source, CollectionFlag::Visible, false);
    scene.setCollections(collections);
    const Entity instancer = scene.createEntity("Instancer");
    registry.emplace<CollectionInstanceComponent>(instancer, source);
    scene.updateWorldTransforms();
    CHECK_MESSAGE(camera.pick(scene, assets, center, 1.0f) == instancer,
                  "a click on a collection instance should pick its instancer");

    scene.destroyEntity(instancer);
    for (const Entity entity : meshes) {
        unlinkFromCollection(scene, entity, source);
    }
    scene.setCollections({});
    CHECK_MESSAGE(camera.pick(scene, assets, center, 1.0f) == picked,
                  "picking should recover once everything is shown");
}

TEST_CASE("EditorCamera: picking stays correct with 2000 additional renderables") {
    PickingScene picking;
    REQUIRE_MESSAGE(static_cast<bool>(picking.model), "camera picking test model should load");
    REQUIRE_MESSAGE(picking.instantiate(), "camera picking test model should instantiate");
    Scene& scene = picking.scene;
    AssetManager& assets = picking.assets;

    const ModelAsset* modelAsset = assets.model(picking.model.handle);
    MeshHandle sharedMesh{};
    if (modelAsset != nullptr) {
        for (const ModelNode& node : modelAsset->nodes) {
            if (node.mesh) {
                sharedMesh = node.mesh;
                break;
            }
        }
    }
    CHECK_MESSAGE(static_cast<bool>(sharedMesh), "camera picking scale test should find a mesh");
    if (sharedMesh) {
        constexpr int objectCount = 2000;
        for (int index = 0; index < objectCount; ++index) {
            const Entity entity = scene.createEntity("Pick target " + std::to_string(index));
            scene.attachMesh(entity, sharedMesh);
            TransformComponent& transform = scene.registry().get<TransformComponent>(entity);
            transform.local[3] = glm::vec4(1000.0f + static_cast<float>(index), 0.0f, 0.0f, 1.0f);
        }
        scene.updateWorldTransforms();
        const auto start = std::chrono::steady_clock::now();
        const Entity picked = picking.camera.pick(scene, assets, {0.5f, 0.5f}, 1.0f);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
        CHECK_MESSAGE((picked != entt::null), "picking should remain correct with 2,000 additional renderables");
        MESSAGE("CPU picking over 2,000 renderables: " << elapsed.count() << " us");
    }
}

TEST_SUITE_END();
