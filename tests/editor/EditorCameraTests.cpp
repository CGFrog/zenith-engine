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
