// Bounded Vulkan smoke test of render views (RenderRuntime::createView): the
// default view renders into the swapchain while two created views render
// offscreen with their own cameras, sizes, and settings. One view resizes,
// one skips frames, one is destroyed and its slot reused, and the default
// view keeps rendering after every created view is gone. Views are isolated
// to one object (local view) for a while, the default one falling back to CPU
// submission, and the scene's editor visibility mode and a collection
// instance change what renders. Run with
// ZENITH_SMOKE_FRAMES (the test registration uses 120) and optionally
// ZENITH_VALIDATION_STRICT=1.

#include "engine/Engine.hpp"
#include "engine/graphics/GraphicsValidation.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include "support/Skip.hpp"
#include "support/StandaloneChecks.hpp"
#include "support/VulkanProbe.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {
// Shared by the scene and main(); the scene is destroyed before run() returns.
struct SmokeResults {
    testsupport::StandaloneChecks checks;
    uint32_t framesWithBothViews{0};
    uint64_t trackedViewBytesDefaultOnly{0};
    uint64_t trackedViewBytesWithViews{0};
    double viewAMilliseconds{0.0};
    double viewBMilliseconds{0.0};
    uint32_t runtimeRenderables{0};
};

// Device memory the renderer reports for size-dependent per-view targets.
uint64_t trackedViewBytes(const RenderStats& stats) {
    return stats.temporalHistoryBytes + stats.motionVectorBytes + stats.ambientOcclusionBytes + stats.reflectionBytes +
           stats.volumetricFogBytes + stats.depthOfFieldBytes + stats.motionBlurBytes + stats.spatialUpscaleBytes;
}

glm::mat4 cameraWorld(const glm::vec3& position, const glm::vec3& target) {
    return glm::inverse(glm::lookAtRH(position, target, glm::vec3(0.0f, 1.0f, 0.0f)));
}

class MultiViewScene final : public ApplicationScene {
  public:
    explicit MultiViewScene(SmokeResults& results)
        : m_results(results) {}

    SceneResult load(ApplicationContext& context) override {
        const AssetLoadResult<MeshHandle> cube = context.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
        if (!cube) {
            return {.success = false, .error = "Cannot load the cube mesh: " + cube.error};
        }
        Scene& scene = context.scene;
        entt::registry& registry = scene.registry();
        for (int x = -2; x <= 2; ++x) {
            for (int z = -2; z <= 2; ++z) {
                const Entity entity = scene.createEntity("Cube");
                registry.get<TransformComponent>(entity).local = glm::translate(
                    glm::mat4(1.0f), glm::vec3(static_cast<float>(x) * 2.5f, 0.5f, static_cast<float>(z) * 2.5f));
                scene.markTransformDirty(entity);
                scene.attachMesh(entity, cube.handle);
                m_cubes.push_back(entity);
            }
        }
        const Entity ground = scene.createEntity("Ground");
        registry.get<TransformComponent>(ground).local = glm::scale(glm::mat4(1.0f), glm::vec3(16.0f, 0.1f, 16.0f));
        scene.markTransformDirty(ground);
        scene.attachMesh(ground, cube.handle);
        const Entity camera = scene.createEntity("Camera");
        registry.get<TransformComponent>(camera).local = cameraWorld({0.0f, 4.0f, 12.0f}, glm::vec3(0.0f));
        scene.markTransformDirty(camera);
        registry.emplace<CameraComponent>(camera);
        scene.markCameraDirty(camera);
        const Entity sun = scene.createEntity("Sun");
        registry.emplace<DirectionalLightComponent>(sun);
        scene.markLightDirty(sun);
        const Entity lamp = scene.createEntity("Lamp");
        registry.get<TransformComponent>(lamp).local = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 1.0f));
        scene.markTransformDirty(lamp);
        registry.emplace<PointLightComponent>(lamp, PointLightComponent{.castsShadows = true});
        scene.markLightDirty(lamp);
        const Entity fog = scene.createEntity("Fog");
        registry.emplace<VolumetricFogComponent>(fog);
        scene.markLightDirty(fog);
        // One cube is drawn again by a collection instance above the grid, and
        // another is hidden in editor views only.
        SceneCollections collections;
        SceneCollectionId instanced;
        if (CollectionResult created = collections.create("Instanced", {}, instanced); !created) {
            return {.success = false, .error = created.error};
        }
        scene.setCollections(std::move(collections));
        linkToCollection(scene, m_cubes.front(), instanced);
        m_instancer = scene.createEntity("Instancer");
        registry.get<TransformComponent>(m_instancer).local =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f));
        scene.markTransformDirty(m_instancer);
        registry.emplace<CollectionInstanceComponent>(m_instancer, instanced);
        registry.emplace<VisibilityComponent>(m_cubes[1], VisibilityComponent{.hidden = true});
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        ++m_frame;
        RenderRuntime& rendering = context.rendering;
        checkPreviousFrame(rendering);
        if (m_frame == 1) {
            createViews(rendering);
        }
        updateIsolation(context);
        if (m_frame == 25) {
            m_results.checks.check(rendering.setActiveView(m_viewB), m_frame, "view B becomes the active view");
        }
        if (m_frame == 50) {
            const RenderObjectId outlined = static_cast<RenderObjectId>(entt::to_integral(m_cubes.front()));
            m_results.checks.check(rendering.setViewOutlinedObjects(m_viewA, std::span(&outlined, 1)), m_frame,
                                   "view A accepts outlines");
        }
        if (m_frame == 60) {
            m_results.checks.check(rendering.destroyView(m_viewB), m_frame, "view B is destroyed");
            m_results.checks.check(rendering.activeView() == rendering.defaultView(), m_frame,
                                   "destroying the active view activates the default view");
        }
        if (m_frame == 70) {
            m_viewC = rendering.createView();
            RenderSettings fxaa = rendering.settings();
            fxaa.antialiasing = AntialiasingMode::Fxaa;
            fxaa.depthPrepass = false;
            fxaa.volumetricFog = VolumetricFogQuality::Off;
            m_results.checks.check(m_viewC && m_viewC.index == m_viewB.index &&
                                       m_viewC.generation != m_viewB.generation,
                                   m_frame, "view C reuses view B's slot with a new generation");
            m_results.checks.check(rendering.setViewSettings(m_viewC, fxaa), m_frame, "view C accepts settings");
            m_results.checks.check(rendering.setViewOverride(m_viewC, RenderView{.position = {-8.0f, 3.0f, 0.0f},
                                                                                 .forward = {1.0f, -0.3f, 0.0f}}),
                                   m_frame, "view C accepts a camera");
        }
        if (m_frame == 90) {
            m_results.checks.check(rendering.destroyView(m_viewA) && rendering.destroyView(m_viewC), m_frame,
                                   "views A and C are destroyed");
        }
        // Views render only on frames that request them; view A skips 80-84.
        const bool viewAHidden = m_frame >= 80 && m_frame < 85;
        if (m_frame < 90 && !viewAHidden) {
            const uint32_t width = m_frame >= 40 ? 400u : 320u;
            const uint32_t height = m_frame >= 40 ? 300u : 180u;
            rendering.requestViewSize(m_viewA, width, height);
        }
        if (m_frame < 60) {
            rendering.requestViewSize(m_viewB, 200, 260);
        }
        if (m_frame >= 70 && m_frame < 90) {
            rendering.requestViewSize(m_viewC, 256, 256);
        }
        // Keep view A's camera moving so its temporal history reprojects.
        const float angle = static_cast<float>(m_frame) * 0.02f;
        rendering.setViewOverride(
            m_viewA, RenderView{.position = {12.0f * std::cos(angle), 5.0f, 12.0f * std::sin(angle)},
                                .forward = glm::normalize(-glm::vec3(std::cos(angle), 0.35f, std::sin(angle))),
                                .verticalFovRadians = glm::radians(45.0f),
                                .nearPlane = 0.2f,
                                .farPlane = 200.0f});
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    RenderObjectId centerCube() const {
        return static_cast<RenderObjectId>(entt::to_integral(m_cubes[m_cubes.size() / 2]));
    }

    // Local view and visibility modes. View B shows only the center cube for
    // frames 10-19 and the default view for 30-39 (CPU submission); editor
    // visibility hides one cube for 45-54; the instancer moves from frame 56.
    void updateIsolation(ApplicationContext& context) {
        RenderRuntime& rendering = context.rendering;
        const std::vector<RenderObjectId> center{centerCube()};
        if (m_frame == 10) {
            m_results.checks.check(rendering.setViewVisibleEntities(m_viewB, center), m_frame,
                                   "view B accepts isolation");
        } else if (m_frame == 20) {
            m_results.checks.check(rendering.setViewVisibleEntities(m_viewB, std::nullopt) &&
                                       !rendering.viewVisibleEntities(m_viewB),
                                   m_frame, "view B leaves isolation");
        } else if (m_frame == 30) {
            m_results.checks.check(rendering.setViewVisibleEntities(rendering.defaultView(), center), m_frame,
                                   "the default view accepts isolation");
        } else if (m_frame == 40) {
            rendering.setViewVisibleEntities(rendering.defaultView(), std::nullopt);
        } else if (m_frame == 45) {
            rendering.setSceneVisibilityMode(SceneVisibilityMode::Editor);
        } else if (m_frame == 55) {
            rendering.setSceneVisibilityMode(SceneVisibilityMode::Runtime);
        }
        if (m_frame >= 56) {
            Scene& scene = context.scene;
            scene.registry().get<TransformComponent>(m_instancer).local =
                glm::translate(glm::mat4(1.0f), glm::vec3(std::sin(static_cast<float>(m_frame) * 0.1f), 3.0f, 0.0f));
            scene.markTransformDirty(m_instancer);
        }
    }

    // Stats of the frame before this update, for the isolation phases.
    void checkIsolation(const RenderRuntime& rendering, uint32_t frame) {
        const uint32_t renderables = rendering.stats().renderableCount;
        if (frame == 5) {
            m_results.runtimeRenderables = renderables;
            // 25 cubes, the ground, and the instanced cube share one mesh.
            m_results.checks.check(renderables > 0 && renderables % 27 == 0, frame,
                                   "the collection instance adds one cube's items");
        }
        if (frame >= 12 && frame < 20) {
            const RenderViewStats b = rendering.viewStats(m_viewB);
            m_results.checks.check(b.visibleCount == 1, frame, "isolated view B draws only the isolated cube");
        }
        if (frame >= 32 && frame < 40) {
            const RenderViewStats isolated = rendering.viewStats(rendering.defaultView());
            m_results.checks.check(isolated.visibleCount == 1, frame, "the isolated default view draws only one cube");
        }
        if (frame >= 47 && frame < 55) {
            m_results.checks.check(renderables * 27 == m_results.runtimeRenderables * 26, frame,
                                   "editor visibility hides the hidden cube");
        }
        if (frame >= 57 && frame < 90) {
            m_results.checks.check(renderables == m_results.runtimeRenderables, frame,
                                   "the runtime mode draws hidden cubes and the moving instance");
        }
    }

    void createViews(RenderRuntime& rendering) {
        m_viewA = rendering.createView();
        m_viewB = rendering.createView();
        m_results.checks.check(m_viewA && m_viewB, m_frame, "two views are created");
        // View A: the full-quality path with its own temporal and fog history.
        RenderSettings full = rendering.settings();
        full.antialiasing = AntialiasingMode::Taa;
        full.ambientOcclusion = AmbientOcclusionQuality::HalfResolution;
        full.reflections = ReflectionQuality::ScreenSpaceHalfResolution;
        full.volumetricFog = VolumetricFogQuality::Medium;
        full.bloom = BloomQuality::Downsampled;
        m_results.checks.check(rendering.setViewSettings(m_viewA, full), m_frame, "view A accepts settings");
        // View B: a cheap top-down view without temporal or screen-space work.
        RenderSettings cheap = rendering.settings();
        cheap.antialiasing = AntialiasingMode::Off;
        cheap.ambientOcclusion = AmbientOcclusionQuality::Off;
        cheap.reflections = ReflectionQuality::ProbesOnly;
        cheap.volumetricFog = VolumetricFogQuality::Off;
        cheap.bloom = BloomQuality::Off;
        cheap.dynamicResolution.enabled = false;
        m_results.checks.check(rendering.setViewSettings(m_viewB, cheap), m_frame, "view B accepts settings");
        m_results.checks.check(rendering.setViewOverride(m_viewB, RenderView{.position = {0.0f, 20.0f, 0.1f},
                                                                             .forward = {0.0f, -1.0f, 0.0f},
                                                                             .up = {0.0f, 0.0f, -1.0f},
                                                                             .verticalFovRadians = glm::radians(70.0f),
                                                                             .nearPlane = 1.0f,
                                                                             .farPlane = 60.0f}),
                               m_frame, "view B accepts a camera");
    }

    // Stats describe the frame submitted before this update.
    void checkPreviousFrame(const RenderRuntime& rendering) {
        const uint32_t frame = m_frame - 1;
        if (frame == 0) {
            return;
        }
        checkIsolation(rendering, frame);
        const RenderViewStats defaultStats = rendering.viewStats(rendering.defaultView());
        m_results.checks.check(defaultStats.rendered, frame, "the default view renders every frame");
        const RenderViewStats a = rendering.viewStats(m_viewA);
        const RenderViewStats b = rendering.viewStats(m_viewB);
        if (frame >= 2 && frame < 40) {
            m_results.checks.check(a.rendered && a.width == 320 && a.height == 180, frame, "view A renders at 320x180");
            m_results.checks.check(rendering.viewTexture(m_viewA) && rendering.viewWidth(m_viewA) == 320, frame,
                                   "view A publishes its texture and size");
        }
        if (frame >= 2 && frame < 60) {
            m_results.checks.check(b.rendered && b.width == 200 && b.height == 260 && b.internalWidth == 200, frame,
                                   "view B renders at 200x260");
            m_results.checks.check(b.visibleCount > 0 && b.drawCalls > 0, frame, "view B draws the scene");
            if (a.rendered && b.rendered) {
                ++m_results.framesWithBothViews;
            }
        }
        if (frame == 30) {
            m_results.trackedViewBytesWithViews = trackedViewBytes(rendering.stats());
            m_results.viewAMilliseconds = a.cpuMilliseconds;
            m_results.viewBMilliseconds = b.cpuMilliseconds;
        }
        if (frame >= 41 && frame < 80) {
            m_results.checks.check(a.rendered && a.width == 400 && a.height == 300, frame, "view A resized to 400x300");
        }
        if (frame >= 81 && frame < 85) {
            m_results.checks.check(!a.rendered, frame, "view A skips frames it was not requested on");
        }
        if (frame >= 61) {
            m_results.checks.check(!rendering.viewAlive(m_viewB), frame, "view B stays destroyed");
        }
        if (frame >= 72 && frame < 90) {
            const RenderViewStats c = rendering.viewStats(m_viewC);
            m_results.checks.check(c.rendered && c.width == 256, frame, "view C renders in the reused slot");
        }
        if (frame >= 91) {
            m_results.checks.check(rendering.viewCount() == 1, frame, "only the default view remains");
        }
        if (frame == 100) {
            m_results.trackedViewBytesDefaultOnly = trackedViewBytes(rendering.stats());
        }
    }

    SmokeResults& m_results;
    std::vector<Entity> m_cubes;
    Entity m_instancer{entt::null};
    RenderViewId m_viewA;
    RenderViewId m_viewB;
    RenderViewId m_viewC;
    uint32_t m_frame{0};
};

class MultiViewApplication final : public Application {
  public:
    explicit MultiViewApplication(SmokeResults& results)
        : m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<MultiViewScene>(m_results);
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
    MultiViewApplication application(results);
    const bool ran = engine.run(application);
    engine.shutdown();
    results.checks.check(results.framesWithBothViews > 0,
                         "no frame rendered both created views (is ZENITH_SMOKE_FRAMES at least 60?)");
    std::cout << "Tracked per-view target bytes: " << results.trackedViewBytesDefaultOnly
              << " with the default view only, " << results.trackedViewBytesWithViews << " with two created views\n";
    std::cout << "View CPU recording: A " << results.viewAMilliseconds << " ms, B " << results.viewBMilliseconds
              << " ms\n";
    const bool passed = ran && results.checks.failures() == 0;
    if (passed) {
        std::cout << "Multi-view smoke test passed\n";
    }
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
