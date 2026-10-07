// Bounded Vulkan smoke test of per-view shadow isolation
// (docs/engine/graphics-views.md, Render views): a view isolated to some
// objects (the editor's local view) casts sun, point, and spot shadows from
// those objects only, while a view
// beside it keeps the whole scene's shadows. First a created view is isolated
// next to the full default view, then the default view is isolated next to the
// full created view. One isolated cube moves every frame so local-light tiles
// keep redrawing. Run with ZENITH_SMOKE_FRAMES (the test registration uses 80),
// ZENITH_DISABLE_SHADOW_CACHE=1 so every cascade redraws each frame, and
// optionally ZENITH_VALIDATION_STRICT=1. With a large ZENITH_SMOKE_FRAMES the
// isolated default view stays on screen, for frame captures.

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
#include <string>
#include <vector>

namespace {
// First frame of each phase, and frames skipped after a change before checks.
constexpr uint32_t createdViewIsolatedFrame = 10;
constexpr uint32_t defaultViewIsolatedFrame = 40;
constexpr uint32_t settleFrames = 3;

struct SmokeResults {
    testsupport::StandaloneChecks checks;
    uint32_t checkedCreatedPhaseFrames{0};
    uint32_t checkedDefaultPhaseFrames{0};
    // Sun and local caster counts of each view, sampled once per phase.
    std::vector<std::string> samples;
};

glm::mat4 cameraWorld(const glm::vec3& position, const glm::vec3& target) {
    return glm::inverse(glm::lookAtRH(position, target, glm::vec3(0.0f, 1.0f, 0.0f)));
}

RenderObjectId objectOf(Entity entity) {
    return static_cast<RenderObjectId>(entt::to_integral(entity));
}

class IsolatedShadowScene final : public ApplicationScene {
  public:
    explicit IsolatedShadowScene(SmokeResults& results)
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
        m_ground = scene.createEntity("Ground");
        registry.get<TransformComponent>(m_ground).local = glm::scale(glm::mat4(1.0f), glm::vec3(16.0f, 0.1f, 16.0f));
        scene.markTransformDirty(m_ground);
        scene.attachMesh(m_ground, cube.handle);
        const Entity camera = scene.createEntity("Camera");
        registry.get<TransformComponent>(camera).local = cameraWorld({0.0f, 7.0f, 11.0f}, glm::vec3(0.0f));
        scene.markTransformDirty(camera);
        registry.emplace<CameraComponent>(camera);
        scene.markCameraDirty(camera);
        const Entity sun = scene.createEntity("Sun");
        registry.emplace<DirectionalLightComponent>(sun);
        scene.markLightDirty(sun);
        const Entity lamp = scene.createEntity("Lamp");
        registry.get<TransformComponent>(lamp).local = glm::translate(glm::mat4(1.0f), glm::vec3(1.2f, 2.5f, 1.2f));
        scene.markTransformDirty(lamp);
        registry.emplace<PointLightComponent>(lamp, PointLightComponent{.intensity = 20.0f, .castsShadows = true});
        scene.markLightDirty(lamp);
        const Entity spot = scene.createEntity("Spot");
        registry.get<TransformComponent>(spot).local = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 4.0f, -1.0f));
        scene.markTransformDirty(spot);
        registry.emplace<SpotLightComponent>(
            spot,
            SpotLightComponent{.intensity = 40.0f, .outerConeRadians = glm::radians(45.0f), .castsShadows = true});
        scene.markLightDirty(spot);
        const Entity fog = scene.createEntity("Fog");
        registry.emplace<VolumetricFogComponent>(fog);
        scene.markLightDirty(fog);
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        ++m_frame;
        RenderRuntime& rendering = context.rendering;
        checkPreviousFrame(rendering);
        if (m_frame == 1) {
            RenderSettings settings = rendering.settings();
            settings.shadows = ShadowQuality::CascadedHigh;
            settings.localShadows = LocalShadowQuality::Medium;
            rendering.setSettings(settings);
            m_view = rendering.createView();
            m_results.checks.check(static_cast<bool>(m_view), m_frame, "a view is created");
            RenderSettings viewSettings = settings;
            viewSettings.volumetricFog = VolumetricFogQuality::Medium;
            m_results.checks.check(rendering.setViewSettings(m_view, viewSettings), m_frame,
                                   "the view accepts settings");
            m_results.checks.check(
                rendering.setViewOverride(m_view, RenderView{.position = {-9.0f, 8.0f, 6.0f},
                                                             .forward = glm::normalize(glm::vec3(9.0f, -8.0f, -6.0f))}),
                m_frame, "the view accepts a camera");
        }
        rendering.requestViewSize(m_view, 320, 240);
        if (m_frame == createdViewIsolatedFrame) {
            m_results.checks.check(rendering.setViewVisibleEntities(m_view, isolation(false)), m_frame,
                                   "the created view accepts isolation");
        }
        if (m_frame == defaultViewIsolatedFrame) {
            m_results.checks.check(rendering.setViewVisibleEntities(m_view, std::nullopt) &&
                                       rendering.setViewVisibleEntities(rendering.defaultView(), isolation(true)),
                                   m_frame, "isolation moves to the default view");
        }
        // The isolated cube moves, so the local-light tiles it touches redraw.
        Scene& scene = context.scene;
        const float offset = std::sin(static_cast<float>(m_frame) * 0.2f) * 0.3f;
        scene.registry().get<TransformComponent>(movingCube()).local =
            glm::translate(glm::mat4(1.0f), glm::vec3(offset, 0.5f, 0.0f));
        scene.markTransformDirty(movingCube());
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    Entity movingCube() const {
        return m_cubes[m_cubes.size() / 2];
    }

    // The created view isolates only the moving cube; the default view (on
    // screen) also keeps the ground, which then receives that cube's shadows only.
    std::vector<RenderObjectId> isolation(bool withGround) const {
        std::vector<RenderObjectId> objects{objectOf(movingCube())};
        if (withGround) {
            objects.push_back(objectOf(m_ground));
        }
        std::ranges::sort(objects);
        return objects;
    }

    // An isolated view's maps hold at most `isolated` casters per redrawn
    // cascade or tile. Only the default view samples the frame's maps.
    void checkIsolatedShadows(const RenderViewStats& stats, uint32_t isolated, bool ownMaps, uint32_t frame,
                              const std::string& view) {
        m_results.checks.check(stats.ownShadowMaps == ownMaps, frame, view + " samples the expected shadow maps");
        m_results.checks.check(stats.shadowCascadesRendered == 3 && stats.shadowCasterCount > 0 &&
                                   stats.shadowCasterCount <= isolated * stats.shadowCascadesRendered,
                               frame, view + " casts sun shadows from its isolated objects only");
        m_results.checks.check(stats.localShadowTilesRendered > 0 && stats.localShadowCasterCount > 0 &&
                                   stats.localShadowCasterCount <= isolated * stats.localShadowTilesRendered,
                               frame, view + " casts point and spot shadows from its isolated objects only");
    }

    // A view of the whole scene beside one isolated to `isolated` objects
    // holds more casters than an isolated view could: the lights' tiles and
    // the sun's cascades see the ground and most cubes.
    void checkFullShadows(const RenderViewStats& stats, uint32_t isolated, bool ownMaps, uint32_t frame,
                          const std::string& view) {
        m_results.checks.check(stats.ownShadowMaps == ownMaps, frame, view + " samples the expected shadow maps");
        m_results.checks.check(stats.localShadowTilesRendered > 0 &&
                                   stats.localShadowCasterCount > isolated * stats.localShadowTilesRendered,
                               frame, view + " casts point and spot shadows from the whole scene");
    }

    void sample(uint32_t frame, const std::string& isolatedName, const RenderViewStats& isolated,
                const std::string& fullName, const RenderViewStats& full) {
        if (frame != 30 && frame != 60) {
            return;
        }
        const auto describe = [](const std::string& name, const RenderViewStats& stats) {
            return name + ": " + std::to_string(stats.shadowCasterCount) + " sun casters in " +
                   std::to_string(stats.shadowCascadesRendered) + " cascades, " +
                   std::to_string(stats.localShadowCasterCount) + " local casters in " +
                   std::to_string(stats.localShadowTilesRendered) + " tiles";
        };
        m_results.samples.push_back("frame " + std::to_string(frame) + " " + describe(isolatedName, isolated) + "; " +
                                    describe(fullName, full));
    }

    // Stats describe the frame submitted before this update.
    void checkPreviousFrame(const RenderRuntime& rendering) {
        const uint32_t frame = m_frame - 1;
        const RenderViewStats defaultStats = rendering.viewStats(rendering.defaultView());
        const RenderViewStats created = rendering.viewStats(m_view);
        if (frame >= 2 && frame < createdViewIsolatedFrame) {
            m_results.checks.check(!defaultStats.ownShadowMaps && created.rendered && !created.ownShadowMaps, frame,
                                   "views without isolation share the frame's shadow maps");
        }
        if (frame >= createdViewIsolatedFrame + settleFrames && frame < defaultViewIsolatedFrame) {
            m_results.checks.check(created.rendered && created.visibleCount == 1, frame,
                                   "the created view is isolated");
            checkIsolatedShadows(created, 1, true, frame, "the isolated created view");
            // The default view's sun casters may be counted by GPU culling of
            // a retired frame, so only its CPU-culled local tiles are checked.
            checkFullShadows(defaultStats, 1, false, frame, "the full default view");
            sample(frame, "isolated created", created, "full default", defaultStats);
            ++m_results.checkedCreatedPhaseFrames;
        }
        if (frame >= defaultViewIsolatedFrame + settleFrames) {
            m_results.checks.check(defaultStats.visibleCount == 2, frame, "the default view is isolated");
            checkIsolatedShadows(defaultStats, 2, false, frame, "the isolated default view");
            m_results.checks.check(created.rendered, frame, "the created view renders");
            checkFullShadows(created, 2, true, frame, "the full created view");
            m_results.checks.check(created.shadowCascadesRendered == 3 && created.shadowCasterCount > 2 * 3, frame,
                                   "the full created view casts sun shadows from the whole scene");
            sample(frame, "isolated default", defaultStats, "full created", created);
            ++m_results.checkedDefaultPhaseFrames;
        }
    }

    SmokeResults& m_results;
    std::vector<Entity> m_cubes;
    Entity m_ground{entt::null};
    RenderViewId m_view;
    uint32_t m_frame{0};
};

class IsolatedShadowApplication final : public Application {
  public:
    explicit IsolatedShadowApplication(SmokeResults& results)
        : m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<IsolatedShadowScene>(m_results);
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
    IsolatedShadowApplication application(results);
    const bool ran = engine.run(application);
    engine.shutdown();
    for (const std::string& sample : results.samples) {
        std::cout << sample << '\n';
    }
    results.checks.check(results.checkedCreatedPhaseFrames > 0 && results.checkedDefaultPhaseFrames > 0,
                         "a phase was never checked (is ZENITH_SMOKE_FRAMES at least 50?)");
    const bool passed = ran && results.checks.failures() == 0;
    if (passed) {
        std::cout << "Isolated shadow smoke test passed\n";
    }
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
