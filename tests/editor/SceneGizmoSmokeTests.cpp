// Bounded Vulkan smoke test of the viewport's scene gizmos: an Engine-hosted
// app builds a fixed scene with one of every gizmo kind (point, spot,
// directional and environment lights, a primary and a second camera, box and
// sphere colliders, a reflection probe, a decal, an irradiance volume) and a
// two-object selection, and every frame records drawSceneGizmos with every
// category set to All into the default view's own debug list, as a viewport
// does. Checks the view publishes the name labels. Run with
// ZENITH_SMOKE_FRAMES (the test registration uses 60) and optionally
// ZENITH_VALIDATION_STRICT=1.
//
// The same executable renders the scene_gizmos reference capture
// (tests/references/scene_gizmos). ZENITH_SCENE_GIZMOS=off records nothing,
// to confirm the comparison notices missing overlays.

#include "editor/viewport/SceneGizmoDraw.hpp"
#include "engine/Engine.hpp"
#include "engine/graphics/GraphicsValidation.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "support/Skip.hpp"
#include "support/StandaloneChecks.hpp"
#include "support/VulkanProbe.hpp"

#include <SDL3/SDL.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct SmokeResults {
    testsupport::StandaloneChecks checks;
    uint32_t framesWithLabels{0};
};

glm::mat4 lookFrom(const glm::vec3& position, const glm::vec3& target) {
    return glm::inverse(glm::lookAtRH(position, target, glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::mat4 translation(const glm::vec3& position) {
    return glm::translate(glm::mat4(1.0f), position);
}

bool gizmosDisabled() {
    const char* value = SDL_getenv("ZENITH_SCENE_GIZMOS");
    return value != nullptr && std::string_view(value) == "off";
}

SceneGizmoOverlays everyOverlay() {
    return {.lights = OverlayScope::All,
            .cameras = OverlayScope::All,
            .colliders = OverlayScope::All,
            .reflectionProbes = OverlayScope::All,
            .decals = OverlayScope::All,
            .irradianceVolumes = OverlayScope::All,
            .names = OverlayScope::All,
            .selectionBounds = true};
}

class SceneGizmoScene final : public ApplicationScene {
  public:
    explicit SceneGizmoScene(SmokeResults& results)
        : m_results(results) {}

    SceneResult load(ApplicationContext& context) override {
        const AssetLoadResult<MeshHandle> cube = context.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
        if (!cube) {
            return {.success = false, .error = "Cannot load the cube mesh: " + cube.error};
        }
        Scene& scene = context.scene;
        entt::registry& registry = scene.registry();
        const auto add = [&](const char* name, const glm::mat4& local) {
            const Entity entity = scene.createEntity(name);
            registry.get<TransformComponent>(entity).local = local;
            scene.markTransformDirty(entity);
            return entity;
        };
        const auto addCube = [&](const char* name, const glm::mat4& local) {
            const Entity entity = add(name, local);
            scene.attachMesh(entity, cube.handle);
            return entity;
        };
        addCube("Floor", glm::scale(glm::mat4(1.0f), glm::vec3(16.0f, 0.1f, 16.0f)));
        const Entity active = addCube("Active", translation({-1.5f, 0.55f, 0.0f}));
        registry.emplace<ColliderComponent>(active);
        const Entity selected =
            addCube("Selected", glm::rotate(translation({1.5f, 0.55f, 0.0f}), glm::radians(30.0f), glm::vec3(0, 1, 0)));
        registry.emplace<ColliderComponent>(selected, ColliderComponent{.shape = SphereCollider{.radius = 0.7f}});
        m_selection = {scene.id(active), scene.id(selected)};

        registry.emplace<PointLightComponent>(add("Point", translation({-4.0f, 1.5f, -2.0f}))).range = 2.0f;
        SpotLightComponent& spot = registry.emplace<SpotLightComponent>(add("Spot", translation({4.0f, 3.0f, -2.0f})));
        spot.range = 3.0f;
        registry.emplace<DirectionalLightComponent>(add("Sun", translation({0.0f, 4.0f, -3.0f})));
        registry.emplace<EnvironmentLightComponent>(add("Sky", translation({-5.5f, 0.5f, 2.0f})));
        registry.emplace<CameraComponent>(add("Main camera", lookFrom({5.0f, 1.5f, 2.5f}, {0.0f, 0.5f, 0.0f})));
        const Entity sideCamera = add("Side camera", lookFrom({-5.0f, 1.5f, 2.5f}, {0.0f, 0.5f, 0.0f}));
        registry.emplace<CameraComponent>(sideCamera).primary = false;
        // A selected camera adds its dotted rays to the icon.
        m_selection.push_back(scene.id(sideCamera));
        registry.emplace<ReflectionProbeComponent>(add("Probe", translation({0.0f, 1.5f, -1.5f}))).halfExtents = {
            2.0f, 1.0f, 1.5f};
        registry
            .emplace<DecalComponent>(
                add("Decal", glm::rotate(translation({0.0f, 0.05f, 2.0f}), glm::radians(20.0f), glm::vec3(0, 1, 0))))
            .halfExtents = {1.0f, 0.25f, 1.0f};
        registry.emplace<IrradianceVolumeComponent>(add("Volume", translation({0.0f, 1.0f, 0.0f}))).halfExtents = {
            7.0f, 2.0f, 4.0f};
        for (const Entity entity : registry.view<const TransformComponent>()) {
            if (registry.any_of<PointLightComponent, SpotLightComponent, DirectionalLightComponent,
                                EnvironmentLightComponent>(entity)) {
                scene.markLightDirty(entity);
            }
            if (registry.all_of<CameraComponent>(entity)) {
                scene.markCameraDirty(entity);
            }
        }
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        ++m_frame;
        RenderRuntime& rendering = context.rendering;
        const RenderViewId view = rendering.defaultView();
        checkPreviousFrame(rendering, view);
        // A fixed editor-style lens, as a viewport overrides its view's camera.
        rendering.setViewOverride(view, RenderView{.position = {0.0f, 6.5f, 11.0f},
                                                   .forward = glm::normalize(glm::vec3(0.0f, -6.0f, -11.0f)),
                                                   .verticalFovRadians = glm::radians(50.0f)});
        DebugDraw* output = rendering.viewDebugDraw(view);
        m_results.checks.check(output != nullptr, m_frame, "the default view has a debug list");
        if (output == nullptr || gizmosDisabled()) {
            return;
        }
        context.scene.updateWorldTransforms();
        drawSceneGizmos({.scene = context.scene, .assets = context.assets, .selection = m_selection}, everyOverlay(),
                        *output);
        m_results.checks.check(output->lineCount() > 0 && !output->labels().empty(), m_frame,
                               "the scene records gizmo lines and labels");
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    // Labels describe the frame drawn before this update.
    void checkPreviousFrame(const RenderRuntime& rendering, RenderViewId view) {
        if (m_frame < 3 || gizmosDisabled()) {
            return;
        }
        m_results.checks.check(rendering.viewDebugDraw(view) == nullptr || rendering.viewDebugDraw(view)->empty(),
                               m_frame, "the renderer clears the view's list after drawing it");
        const std::span<const ProjectedDebugLabel> labels = rendering.viewDebugLabels(view);
        const bool hasActive = std::ranges::any_of(labels, [](const ProjectedDebugLabel& label) {
            return label.text == "Active" && label.color == sceneGizmoColors().active;
        });
        if (hasActive) {
            ++m_results.framesWithLabels;
        }
    }

    SmokeResults& m_results;
    std::vector<SceneEntityId> m_selection;
    uint32_t m_frame{0};
};

class SceneGizmoApplication final : public Application {
  public:
    explicit SceneGizmoApplication(SmokeResults& results)
        : m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<SceneGizmoScene>(m_results);
    }

  private:
    SmokeResults& m_results;
};
} // namespace

int main(int, char** argv) {
    initializeResourceLocator(argv[0]);
    Engine engine;
    if (!engine.init()) {
        std::cerr << "Engine initialization failed\n";
        engine.shutdown();
        if (std::string reason; !testsupport::vulkanDeviceAvailable(testsupport::VulkanDeviceKind::Any, reason)) {
            std::cout << "Skipping: " << reason << '\n';
            return testsupport::skipExitCode;
        }
        return validationExitCode(engine.graphicsValidation(), EXIT_FAILURE);
    }
    SmokeResults results;
    SceneGizmoApplication application(results);
    const bool ran = engine.run(application);
    engine.shutdown();
    results.checks.check(gizmosDisabled() || results.framesWithLabels > 0,
                         "the view never published the active object's label (is ZENITH_SMOKE_FRAMES at least 4?)");
    const bool passed = ran && results.checks.failures() == 0;
    if (passed) {
        std::cout << "Scene gizmo smoke test passed\n";
    }
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
