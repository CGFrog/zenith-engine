// What secondary render scenes (RenderRuntime::createScene, the editor's
// prefab windows) cost per frame. The application's scene (a lit grid of
// cubes) renders in the default view; up to RenderRuntime::maximumScenes
// secondary scenes, each `ZENITH_SECONDARY_BENCHMARK_OBJECTS` cubes (500 by
// default), have a 320x240 view each. For each count N of bound scenes in
// {0, 1, 2, 4, 8} it measures three phases:
//
//   static  every bound scene is drawn and nothing changes;
//   moving  as static, one object of the first scene moves every frame;
//   hidden  every bound scene is bound but no view is requested (a hidden
//           prefab window), so nothing should be drawn.
//
// Each phase runs `ZENITH_SECONDARY_BENCHMARK_WARMUP` frames (10) unmeasured,
// then `ZENITH_SECONDARY_BENCHMARK_FRAMES` (120) measured, and samples:
//   n<N>.<phase>.renderer        the renderer's CPU time for the frame
//                                (EngineFrameTimings), minus its fence wait,
//                                image acquire, and present;
//   n<N>.<phase>.secondary       the part spent on secondary scenes
//                                (RenderStats::cpuSecondarySceneMilliseconds);
//   n<N>.<phase>.rebuilds        secondary scenes that rebuilt their packets.
// The JSON report goes to stdout. CTest runs it with a few frames only to
// check it completes; run it by hand (on a real GPU) for numbers.

#include "engine/Engine.hpp"
#include "engine/diagnostics/Benchmark.hpp"
#include "engine/diagnostics/FrameTimings.hpp"
#include "engine/graphics/GraphicsValidation.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr std::array<uint32_t, 5> sceneCounts{0, 1, 2, 4, 8};
constexpr std::array<const char*, 3> phaseNames{"static", "moving", "hidden"};
static_assert(sceneCounts.back() <= RenderRuntime::maximumScenes);

uint32_t environmentCount(const char* name, uint32_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr) {
        return fallback;
    }
    const long value = std::strtol(text, nullptr, 10);
    return value > 0 ? static_cast<uint32_t>(value) : fallback;
}

struct BenchmarkSettings {
    uint32_t objects{environmentCount("ZENITH_SECONDARY_BENCHMARK_OBJECTS", 500)};
    uint32_t warmupFrames{environmentCount("ZENITH_SECONDARY_BENCHMARK_WARMUP", 10)};
    uint32_t measuredFrames{environmentCount("ZENITH_SECONDARY_BENCHMARK_FRAMES", 120)};
};

struct BenchmarkResults {
    BenchmarkReport report;
    std::vector<std::string> failures;
    bool finished{false};
};

Entity addCube(Scene& scene, MeshHandle cube, const glm::vec3& position) {
    const Entity entity = scene.createEntity("Cube");
    scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
    scene.markTransformDirty(entity);
    scene.attachMesh(entity, cube);
    return entity;
}

// count cubes in a square grid on the XZ plane, centred on the origin.
void addCubeGrid(Scene& scene, MeshHandle cube, uint32_t count, float spacing) {
    const auto side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(count))));
    const float half = static_cast<float>(side - 1) * spacing * 0.5f;
    for (uint32_t index = 0; index < count; ++index) {
        addCube(scene, cube,
                {static_cast<float>(index % side) * spacing - half, 0.0f,
                 static_cast<float>(index / side) * spacing - half});
    }
}

class SecondarySceneBenchmark final : public ApplicationScene {
  public:
    SecondarySceneBenchmark(const BenchmarkSettings& settings, BenchmarkResults& results)
        : m_settings(settings)
        , m_results(results) {}

    SceneResult load(ApplicationContext& context) override {
        const AssetLoadResult<MeshHandle> cube = context.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
        if (!cube) {
            return {.success = false, .error = "Cannot load the cube mesh: " + cube.error};
        }
        Scene& scene = context.scene;
        addCubeGrid(scene, cube.handle, 64, 2.5f);
        const Entity camera = scene.createEntity("Camera");
        scene.registry().get<TransformComponent>(camera).local =
            glm::inverse(glm::lookAtRH(glm::vec3(0.0f, 12.0f, 24.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
        scene.markTransformDirty(camera);
        scene.registry().emplace<CameraComponent>(camera);
        scene.markCameraDirty(camera);
        const Entity sun = scene.createEntity("Sun");
        scene.registry().emplace<DirectionalLightComponent>(sun);
        scene.markLightDirty(sun);
        for (Scene& secondary : m_secondary) {
            addCubeGrid(secondary, cube.handle, m_settings.objects, 1.5f);
            secondary.updateWorldTransforms();
        }
        m_mover = *m_secondary.front().registry().view<const MeshRendererComponent>().begin();
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        RenderRuntime& rendering = context.rendering;
        if (!m_created) {
            create(rendering);
            m_created = true;
        } else {
            measurePreviousFrame(context);
        }
        if (!advance(context)) {
            return;
        }
        const uint32_t bound = sceneCounts[m_count];
        const bool hidden = m_phase == 2;
        if (m_phase == 1) {
            // One object of the first scene moves every frame.
            Scene& first = m_secondary.front();
            const float offset = static_cast<float>(m_frame % 60) * 0.05f;
            first.registry().get<TransformComponent>(m_mover).local =
                glm::translate(glm::mat4(1.0f), glm::vec3(offset, 1.0f, 0.0f));
            first.markTransformDirty(m_mover);
        }
        for (uint32_t index = 0; index < bound; ++index) {
            m_secondary[index].updateWorldTransforms();
            rendering.setSceneContent(m_scenes[index], &m_secondary[index]);
            if (!hidden) {
                rendering.requestViewSize(m_views[index], 320, 240);
            }
        }
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    void create(RenderRuntime& rendering) {
        for (size_t index = 0; index < m_secondary.size(); ++index) {
            m_views[index] = rendering.createView();
            m_scenes[index] = rendering.createScene();
            if (!m_views[index] || !m_scenes[index] || !rendering.setViewScene(m_views[index], m_scenes[index]) ||
                !rendering.setViewOverride(m_views[index],
                                           RenderView{.position = {0.0f, 25.0f, 35.0f},
                                                      .forward = glm::normalize(glm::vec3(0.0f, -0.6f, -1.0f)),
                                                      .verticalFovRadians = glm::radians(50.0f),
                                                      .nearPlane = 0.1f,
                                                      .farPlane = 200.0f})) {
                m_results.failures.push_back("could not create secondary view " + std::to_string(index));
            }
        }
    }

    // Moves to the next phase or scene count when the current one has run
    // its frames; false once every phase has run (the run then ends).
    bool advance(ApplicationContext& context) {
        if (m_frame == m_settings.warmupFrames + m_settings.measuredFrames) {
            m_frame = 0;
            if (++m_phase == phaseNames.size()) {
                m_phase = 0;
                ++m_count;
            }
        }
        if (m_count == sceneCounts.size()) {
            m_results.finished = true;
            context.control.requestQuit();
            return false;
        }
        ++m_frame;
        m_measuring = m_frame > m_settings.warmupFrames;
        return true;
    }

    // Stats describe the frame submitted after the previous update.
    void measurePreviousFrame(const ApplicationContext& context) {
        if (!m_measuring || context.frameTimings == nullptr) {
            return;
        }
        const RenderStats& stats = context.rendering.stats();
        const std::string prefix =
            "n" + std::to_string(sceneCounts[m_count]) + "." + std::string(phaseNames[m_phase]) + ".";
        const double renderer = context.frameTimings->rendererMilliseconds - stats.cpuFrameWaitMilliseconds -
                                stats.cpuAcquireMilliseconds - stats.cpuPresentMilliseconds;
        m_results.report.sample(prefix + "renderer", renderer);
        m_results.report.sample(prefix + "secondary", stats.cpuSecondarySceneMilliseconds);
        m_results.report.sample(prefix + "rebuilds", stats.secondaryScenePacketRebuilds, "count");
        const uint32_t expected = m_phase == 2 ? 0 : sceneCounts[m_count];
        if (stats.secondaryScenesDrawn != expected) {
            m_results.failures.push_back(prefix + ": " + std::to_string(stats.secondaryScenesDrawn) +
                                         " secondary scenes drawn, expected " + std::to_string(expected));
        }
    }

    const BenchmarkSettings& m_settings;
    BenchmarkResults& m_results;
    std::array<Scene, sceneCounts.back()> m_secondary;
    std::array<RenderViewId, sceneCounts.back()> m_views;
    std::array<RenderSceneId, sceneCounts.back()> m_scenes;
    Entity m_mover{entt::null};
    bool m_created{false};
    bool m_measuring{false};
    size_t m_count{0};
    size_t m_phase{0};
    uint32_t m_frame{0};
};

class SecondarySceneBenchmarkApplication final : public Application {
  public:
    SecondarySceneBenchmarkApplication(const BenchmarkSettings& settings, BenchmarkResults& results)
        : m_settings(settings)
        , m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<SecondarySceneBenchmark>(m_settings, m_results);
    }

  private:
    const BenchmarkSettings& m_settings;
    BenchmarkResults& m_results;
};
} // namespace

int main(int, char** argv) {
    initializeResourceLocator(argv[0]);
    Engine engine;
    if (!engine.init()) {
        std::cerr << "Engine initialization failed\n";
        engine.shutdown();
        return validationExitCode(engine.graphicsValidation(), EXIT_FAILURE);
    }
    const BenchmarkSettings settings;
    BenchmarkResults results;
    addBuildMetadata(results.report);
    results.report.metadata("scenario", "secondary_scenes");
    results.report.metadata("objects_per_scene", std::to_string(settings.objects));
    results.report.metadata("measured_frames", std::to_string(settings.measuredFrames));
    SecondarySceneBenchmarkApplication application(settings, results);
    const bool ran = engine.run(application);
    engine.shutdown();
    for (const std::string& failure : results.failures) {
        std::cerr << "FAILED: " << failure << '\n';
    }
    if (!results.finished) {
        std::cerr << "FAILED: the run ended before every phase ran\n";
    }
    std::cout << results.report.json(false) << '\n';
    const bool passed = ran && results.finished && results.failures.empty();
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
