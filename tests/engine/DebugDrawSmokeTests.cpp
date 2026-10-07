// Bounded Vulkan smoke test of debug drawing (RenderRuntime::debugDraw):
// every frame records a fixed primitive set for all views plus per-view
// lists, while the default view renders into the swapchain and two created
// views render offscreen (one upscaled, so its depth is at a lower render
// resolution than its lines). Checks the one-frame lifetime of the lists and
// the projected labels each view publishes. Run with ZENITH_SMOKE_FRAMES (the
// test registration uses 60) and optionally ZENITH_VALIDATION_STRICT=1.
//
// The same executable renders the debug_draw reference capture
// (tests/references/debug_draw): the primitive set and camera are fixed, so a
// capture of the default view is deterministic.

#include "engine/Engine.hpp"
#include "engine/graphics/GraphicsValidation.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include "support/Skip.hpp"
#include "support/StandaloneChecks.hpp"
#include "support/VulkanProbe.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {
struct SmokeResults {
    testsupport::StandaloneChecks checks;
    uint32_t framesWithLabels{0};
};

glm::mat4 cameraWorld(const glm::vec3& position, const glm::vec3& target) {
    return glm::inverse(glm::lookAtRH(position, target, glm::vec3(0.0f, 1.0f, 0.0f)));
}

bool hasLabel(std::span<const ProjectedDebugLabel> labels, const std::string& text) {
    return std::ranges::any_of(labels, [&](const ProjectedDebugLabel& label) { return label.text == text; });
}

// The fixed primitive set: one of each primitive, depth-tested ones placed
// to intersect or pass behind the cubes, and on-top ones crossing them.
void recordPrimitives(DebugDraw& draw) {
    const DebugStyle yellow{.color = {1.0f, 0.85f, 0.1f, 1.0f}};
    const DebugStyle cyan{.color = {0.1f, 0.9f, 1.0f, 1.0f}};
    const DebugStyle magenta{.color = {1.0f, 0.2f, 0.9f, 1.0f}};
    const DebugStyle green{.color = {0.3f, 1.0f, 0.3f, 1.0f}};
    const DebugStyle orange{.color = {1.0f, 0.5f, 0.1f, 1.0f}};
    const DebugStyle white{.color = {1.0f, 1.0f, 1.0f, 1.0f}};
    const DebugStyle onTopBlue{.color = {0.2f, 0.4f, 1.0f, 1.0f}, .depth = DebugDepth::AlwaysOnTop};
    const DebugStyle onTopRed{.color = {1.0f, 0.15f, 0.1f, 1.0f}, .depth = DebugDepth::AlwaysOnTop};
    const DebugStyle translucent{.color = {1.0f, 1.0f, 1.0f, 0.45f}};

    draw.aabb({-2.6f, -0.05f, -0.6f}, {-1.4f, 1.15f, 0.6f}, yellow);
    draw.box({2.0f, 0.6f, 0.0f}, {0.75f, 0.6f, 0.75f}, glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0)), cyan);
    // Half inside the centre cube: its lower part is hidden.
    draw.sphere({0.0f, 1.0f, -1.5f}, 0.9f, magenta);
    draw.capsule({3.6f, 0.5f, -2.5f}, {3.6f, 2.0f, -2.5f}, 0.4f, green);
    draw.cone({-3.6f, 2.6f, -2.5f}, {0.0f, -1.0f, 0.0f}, 2.0f, glm::radians(25.0f), orange);
    glm::mat4 probeProjection = glm::perspectiveRH_ZO(glm::radians(50.0f), 1.5f, 2.5f, 0.3f);
    probeProjection[1][1] *= -1.0f;
    const glm::mat4 probeView =
        glm::lookAtRH(glm::vec3(-0.5f, 1.2f, 3.0f), glm::vec3(1.0f, 0.6f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    draw.frustum(probeProjection * probeView, white);
    draw.circle({0.0f, 0.06f, 0.0f}, {0.0f, 1.0f, 0.0f}, 3.2f, translucent, 64);
    draw.arrow({-1.0f, 0.06f, 2.2f}, {1.5f, 0.06f, 2.2f}, onTopRed);
    // Through all three cubes, visible over them.
    draw.line({-3.5f, 0.5f, 0.0f}, {3.5f, 0.5f, 0.0f}, onTopBlue);
    draw.label({0.0f, 0.0f, 0.0f}, "origin");
    // Behind the default camera: never projected.
    draw.label({0.0f, 3.5f, 20.0f}, "behind");
}

class DebugDrawScene final : public ApplicationScene {
  public:
    explicit DebugDrawScene(SmokeResults& results)
        : m_results(results) {}

    SceneResult load(ApplicationContext& context) override {
        const AssetLoadResult<MeshHandle> cube = context.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
        if (!cube) {
            return {.success = false, .error = "Cannot load the cube mesh: " + cube.error};
        }
        Scene& scene = context.scene;
        entt::registry& registry = scene.registry();
        const auto addCube = [&](const glm::mat4& transform) {
            const Entity entity = scene.createEntity("Cube");
            registry.get<TransformComponent>(entity).local = transform;
            scene.markTransformDirty(entity);
            scene.attachMesh(entity, cube.handle);
        };
        for (const glm::vec3& position :
             {glm::vec3(-2.0f, 0.55f, 0.0f), glm::vec3(2.0f, 0.55f, 0.0f), glm::vec3(0.0f, 0.55f, -1.5f)}) {
            addCube(glm::translate(glm::mat4(1.0f), position));
        }
        addCube(glm::scale(glm::mat4(1.0f), glm::vec3(16.0f, 0.1f, 16.0f)));
        const Entity camera = scene.createEntity("Camera");
        registry.get<TransformComponent>(camera).local = cameraWorld({0.0f, 3.5f, 8.0f}, {0.0f, 0.6f, 0.0f});
        scene.markTransformDirty(camera);
        registry.emplace<CameraComponent>(camera);
        scene.markCameraDirty(camera);
        const Entity sun = scene.createEntity("Sun");
        registry.get<TransformComponent>(sun).local =
            glm::inverse(glm::lookAtRH(glm::vec3(0.0f), glm::vec3(-0.4f, -1.0f, -0.6f), glm::vec3(0, 1, 0)));
        scene.markTransformDirty(sun);
        registry.emplace<DirectionalLightComponent>(sun);
        scene.markLightDirty(sun);
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        ++m_frame;
        RenderRuntime& rendering = context.rendering;
        checkPreviousFrame(rendering);
        if (m_frame == 1) {
            createViews(rendering);
        }
        rendering.requestViewSize(m_upscaledView, 320, 180);
        rendering.requestViewSize(m_topView, 200, 200);
        recordPrimitives(rendering.debugDraw());
        if (DebugDraw* own = rendering.viewDebugDraw(m_upscaledView)) {
            own->box({0.0f, 0.55f, -1.5f}, glm::vec3(0.6f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                     {.color = {1.0f, 0.0f, 0.0f, 1.0f}});
            own->label({0.0f, 1.2f, -1.5f}, "upscaled only");
        }
        if (DebugDraw* own = rendering.viewDebugDraw(m_topView)) {
            own->circle({0.0f, 0.1f, 0.0f}, {0.0f, 1.0f, 0.0f}, 5.0f, {.depth = DebugDepth::AlwaysOnTop});
        }
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    void createViews(RenderRuntime& rendering) {
        m_upscaledView = rendering.createView();
        m_topView = rendering.createView();
        m_results.checks.check(m_upscaledView && m_topView, m_frame, "two views are created");
        RenderSettings upscaled = rendering.settings();
        upscaled.antialiasing = AntialiasingMode::Taa;
        upscaled.upscaler.mode = UpscalerMode::Fsr1;
        upscaled.upscaler.quality = UpscalerQuality::Performance;
        m_results.checks.check(rendering.setViewSettings(m_upscaledView, upscaled), m_frame, "upscaled view settings");
        m_results.checks.check(rendering.setViewOverride(
                                   m_upscaledView, RenderView{.position = {5.0f, 3.0f, 6.0f},
                                                              .forward = glm::normalize(glm::vec3(-5.0f, -2.4f, -6.0f)),
                                                              .verticalFovRadians = glm::radians(50.0f)}),
                               m_frame, "upscaled view camera");
        m_results.checks.check(rendering.setViewOverride(m_topView, RenderView{.position = {0.0f, 12.0f, 0.1f},
                                                                               .forward = {0.0f, -1.0f, 0.0f},
                                                                               .up = {0.0f, 0.0f, -1.0f},
                                                                               .nearPlane = 1.0f,
                                                                               .farPlane = 40.0f}),
                               m_frame, "top view camera");
    }

    // Labels and stats describe the frame drawn before this update.
    void checkPreviousFrame(const RenderRuntime& rendering) {
        const uint32_t frame = m_frame - 1;
        if (frame == 0) {
            return;
        }
        m_results.checks.check(rendering.debugDraw().empty(), frame, "the renderer clears the all-views list");
        const DebugDraw* upscaledList = rendering.viewDebugDraw(m_upscaledView);
        m_results.checks.check(upscaledList == nullptr || upscaledList->empty(), frame,
                               "the renderer clears view lists");
        const std::span<const ProjectedDebugLabel> defaultLabels = rendering.viewDebugLabels(rendering.defaultView());
        m_results.checks.check(hasLabel(defaultLabels, "origin"), frame, "the default view projects the shared label");
        m_results.checks.check(!hasLabel(defaultLabels, "behind"), frame, "a label behind the camera is culled");
        m_results.checks.check(!hasLabel(defaultLabels, "upscaled only"), frame, "view labels stay in their view");
        const RenderViewStats defaultStats = rendering.viewStats(rendering.defaultView());
        for (const ProjectedDebugLabel& label : defaultLabels) {
            m_results.checks.check(label.position.x >= 0.0f && label.position.y >= 0.0f &&
                                       label.position.x <= static_cast<float>(defaultStats.width) &&
                                       label.position.y <= static_cast<float>(defaultStats.height) &&
                                       label.distance > 0.0f,
                                   frame, "projected labels lie inside the view");
        }
        if (frame >= 2) {
            const RenderViewStats upscaled = rendering.viewStats(m_upscaledView);
            m_results.checks.check(upscaled.rendered && rendering.viewStats(m_topView).rendered, frame,
                                   "both created views render");
            const std::span<const ProjectedDebugLabel> upscaledLabels = rendering.viewDebugLabels(m_upscaledView);
            m_results.checks.check(hasLabel(upscaledLabels, "origin") && hasLabel(upscaledLabels, "upscaled only"),
                                   frame, "a created view projects shared and own labels");
            if (hasLabel(defaultLabels, "origin") && hasLabel(upscaledLabels, "upscaled only")) {
                ++m_results.framesWithLabels;
            }
        }
    }

    SmokeResults& m_results;
    RenderViewId m_upscaledView;
    RenderViewId m_topView;
    uint32_t m_frame{0};
};

class DebugDrawApplication final : public Application {
  public:
    explicit DebugDrawApplication(SmokeResults& results)
        : m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<DebugDrawScene>(m_results);
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
    DebugDrawApplication application(results);
    const bool ran = engine.run(application);
    engine.shutdown();
    results.checks.check(results.framesWithLabels > 0,
                         "no frame published labels in two views (is ZENITH_SMOKE_FRAMES at least 4?)");
    const bool passed = ran && results.checks.failures() == 0;
    if (passed) {
        std::cout << "Debug draw smoke test passed\n";
    }
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
