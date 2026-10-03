#include "editor/application/EditorApplication.hpp"
#include "editor/interaction/EditorCamera.hpp"

#include <chrono>
#include <cmath>
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

bool near(glm::vec3 left, glm::vec3 right, float tolerance = 0.0001f) {
    return glm::length(left - right) <= tolerance;
}
} // namespace

int main() {
    EditorApplication editor({}, {}, {});
    expect(editor.mouseCapturePolicy() == MouseCapturePolicy::WhileRightMouseDown,
           "the editor should only capture the cursor during right-button freelook");

    EditorCamera camera;
    EditorCamera horizontalRotation;
    InputState rotateRight;
    rotateRight.setMouseButtonDown(MouseButton::Right);
    rotateRight.mouseDelta.x = 40.0f;
    horizontalRotation.update(rotateRight, 0.0f, true);
    expect(horizontalRotation.renderView().forward.x > 0.0f,
           "dragging right should rotate the editor camera toward the right");

    camera.setSpeed(-1.0f);
    expect(camera.speed() == 0.05f, "camera speed should clamp to its positive minimum");
    camera.setSpeed(2000.0f);
    expect(camera.speed() == 1000.0f, "camera speed should clamp to its maximum");
    camera.focus({0.0f, 0.0f, 0.0f}, 2.0f);
    const RenderView view = camera.renderView();
    expect(glm::dot(glm::normalize(-view.position), view.forward) > 0.999f,
           "focus should point the camera at the requested world position");

    InputState select;
    select.setMouseButtonDown(MouseButton::Left);
    select.mouseDelta = {80.0f, 40.0f};
    camera.update(select, 1.0f / 60.0f, true);
    expect(near(camera.renderView().position, view.position) && near(camera.renderView().forward, view.forward),
           "left dragging should not navigate the editor camera");

    InputState orbit;
    orbit.setMouseButtonDown(MouseButton::Middle);
    orbit.mouseDelta = {80.0f, 40.0f};
    camera.update(orbit, 1.0f / 60.0f, true);
    expect(!near(camera.renderView().forward, view.forward), "middle dragging should orbit around the pivot");

    const RenderView beforePan = camera.renderView();
    InputState pan;
    pan.setMouseButtonDown(MouseButton::Middle);
    pan.setKeyDown(Key::LeftShift);
    pan.mouseDelta = {80.0f, 40.0f};
    camera.update(pan, 1.0f / 60.0f, true);
    expect(!near(camera.renderView().position, beforePan.position) &&
               near(camera.renderView().forward, beforePan.forward),
           "Shift+middle dragging should pan without rotating");

    const glm::vec3 beforeZoom = camera.renderView().position;
    InputState zoom;
    zoom.mouseWheelDelta.y = 1.0f;
    camera.update(zoom, 1.0f / 60.0f, true);
    expect(!near(camera.renderView().position, beforeZoom), "mouse wheel should zoom toward the orbit pivot");

    const RenderView beforeInactiveWasd = camera.renderView();
    InputState inactiveWasd;
    inactiveWasd.setKeyDown(Key::W);
    camera.update(inactiveWasd, 1.0f, true);
    expect(near(camera.renderView().position, beforeInactiveWasd.position),
           "WASD should not move the editor camera outside right-button freelook");

    InputState freelook;
    freelook.setMouseButtonDown(MouseButton::Right);
    freelook.setKeyDown(Key::W);
    freelook.mouseDelta = {40.0f, 10.0f};
    camera.update(freelook, 1.0f, true);
    expect(!near(camera.renderView().position, beforeInactiveWasd.position) &&
               !near(camera.renderView().forward, beforeInactiveWasd.forward),
           "right-button freelook should rotate the camera and enable WASD movement");

    expect(camera.projectionMatrix(1.0f)[1][1] < 0.0f && camera.gizmoProjectionMatrix(1.0f)[1][1] > 0.0f,
           "rendering and screen-space gizmos should use their respective vertical projection conventions");

    // The lens fields feed the render view and both projections alike.
    EditorCamera lens;
    expect(std::abs(lens.verticalFovRadians() - glm::radians(60.0f)) < 0.0001f && lens.nearPlane() == 0.1f &&
               lens.farPlane() == 1000.0f,
           "the editor camera should keep the previous 60 degree, 0.1 to 1000 lens by default");
    const float defaultFocalScale = lens.projectionMatrix(1.0f)[0][0];
    lens.setVerticalFovRadians(glm::radians(30.0f));
    lens.setClipPlanes(0.5f, 250.0f);
    const RenderView lensView = lens.renderView();
    expect(std::abs(lensView.verticalFovRadians - glm::radians(30.0f)) < 0.0001f && lensView.nearPlane == 0.5f &&
               lensView.farPlane == 250.0f,
           "the render view should carry the camera's field of view and clip planes");
    const glm::mat4 narrow = lens.projectionMatrix(1.0f);
    const float expectedFocalScale = 1.0f / std::tan(glm::radians(15.0f));
    expect(std::abs(narrow[0][0] - expectedFocalScale) < 0.0001f && narrow[0][0] > defaultFocalScale &&
               std::abs(lens.gizmoProjectionMatrix(1.0f)[1][1] - expectedFocalScale) < 0.0001f,
           "a narrower field of view should magnify both the render and gizmo projections");
    const glm::vec4 nearClip = narrow * glm::vec4(0.0f, 0.0f, -0.5f, 1.0f);
    const glm::vec4 farClip = narrow * glm::vec4(0.0f, 0.0f, -250.0f, 1.0f);
    expect(std::abs(nearClip.z / nearClip.w) < 0.0001f && std::abs(farClip.z / farClip.w - 1.0f) < 0.0001f,
           "the render projection should map the clip planes to depth 0 and 1");
    lens.setVerticalFovRadians(10.0f);
    expect(lens.verticalFovRadians() < glm::radians(180.0f), "the field of view should clamp below 180 degrees");
    lens.setVerticalFovRadians(-1.0f);
    expect(lens.verticalFovRadians() > 0.0f, "the field of view should clamp above zero");
    lens.setClipPlanes(-1.0f, -5.0f);
    expect(lens.nearPlane() > 0.0f && lens.farPlane() > lens.nearPlane(),
           "invalid clip planes should clamp to a positive, non-empty depth range");
    lens.setClipPlanes(10.0f, 2.0f);
    expect(lens.nearPlane() == 10.0f && lens.farPlane() > 10.0f,
           "a far plane before the near plane should move past it");

    camera.focus({0.0f, 0.0f, 0.0f}, 2.0f);

    AssetManager assets;
    const AssetLoadResult<ModelHandle> model =
        assets.loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf");
    expect(static_cast<bool>(model), "camera picking test model should load");
    Scene scene;
    const Entity root = scene.createEntity("Model");
    if (model) {
        expect(scene.instantiateModel(root, model.handle, assets).success,
               "camera picking test model should instantiate");
        scene.updateWorldTransforms();
        expect(camera.pick(scene, assets, {0.5f, 0.5f}, 1.0f) != entt::null,
               "center viewport ray should select visible model geometry");

        // Picking follows the lens: a point near the image edge looks almost
        // straight ahead through a narrow lens and hits the model, while the
        // same point through a wide lens looks far past it.
        const glm::vec2 edge{0.5f, 0.1f};
        EditorCamera wideLens = camera;
        wideLens.setVerticalFovRadians(glm::radians(120.0f));
        EditorCamera narrowLens = camera;
        narrowLens.setVerticalFovRadians(glm::radians(5.0f));
        expect(narrowLens.pick(scene, assets, edge, 1.0f) != entt::null &&
                   wideLens.pick(scene, assets, edge, 1.0f) == entt::null,
               "picking should cast through the camera's field of view");

        const ModelAsset* modelAsset = assets.model(model.handle);
        MeshHandle sharedMesh{};
        if (modelAsset != nullptr) {
            for (const ModelNode& node : modelAsset->nodes) {
                if (node.mesh) {
                    sharedMesh = node.mesh;
                    break;
                }
            }
        }
        expect(static_cast<bool>(sharedMesh), "camera picking scale test should find a mesh");
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
            const Entity picked = camera.pick(scene, assets, {0.5f, 0.5f}, 1.0f);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
            expect(picked != entt::null, "picking should remain correct with 2,000 additional renderables");
            std::cout << "CPU picking over 2,000 renderables: " << elapsed.count() << " us\n";
        }
    }
    return failures == 0 ? 0 : 1;
}
