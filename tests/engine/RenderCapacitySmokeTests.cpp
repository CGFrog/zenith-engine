// Bounded Vulkan smoke test of the renderer's per-frame capacities: a scene
// with more renderables than the per-frame object capacity and more particle
// emitters than particles::maximumEmitters must keep rendering, draw the
// capacity's worth, count the excess in RenderStats, and log each overflow
// once rather than every frame. Run with ZENITH_SMOKE_FRAMES (the
// registration uses 20).

#include "engine/Engine.hpp"
#include "engine/graphics/GraphicsValidation.hpp"
#include "engine/graphics/Particles.hpp"
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
#include <vector>

namespace {
// The renderer's per-frame object capacity (maximumObjectsPerFrame).
constexpr uint32_t objectCapacity = 16384;
constexpr uint32_t extraCubes = 500;
constexpr uint32_t extraEmitters = 6;

struct SmokeResults {
    testsupport::StandaloneChecks checks;
    uint32_t primitivesPerCube{0};
    uint32_t checkedFrames{0};
};

class CapacityScene final : public ApplicationScene {
  public:
    explicit CapacityScene(SmokeResults& results)
        : m_results(results) {}

    SceneResult load(ApplicationContext& context) override {
        const AssetLoadResult<MeshHandle> cube = context.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
        if (!cube) {
            return {.success = false, .error = "Cannot load the cube mesh: " + cube.error};
        }
        m_results.primitivesPerCube = static_cast<uint32_t>(context.assets.mesh(cube.handle)->primitives.size());
        Scene& scene = context.scene;
        entt::registry& registry = scene.registry();
        const uint32_t cubes = objectCapacity / std::max(m_results.primitivesPerCube, 1u) + extraCubes;
        for (uint32_t index = 0; index < cubes; ++index) {
            const Entity entity = scene.createEntity("Cube");
            const glm::vec3 position(static_cast<float>(index % 128) * 3.0f, static_cast<float>(index / 128) * 3.0f,
                                     0.0f);
            registry.get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
            scene.markTransformDirty(entity);
            scene.attachMesh(entity, cube.handle);
        }
        for (uint32_t index = 0; index < particles::maximumEmitters + extraEmitters; ++index) {
            const Entity emitter = scene.createEntity("Emitter");
            registry.get<TransformComponent>(emitter).local =
                glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(index), 0.0f, 4.0f));
            scene.markTransformDirty(emitter);
            registry.emplace<ParticleEmitterComponent>(emitter, ParticleEmitterComponent{.maxParticles = 8});
        }
        const Entity camera = scene.createEntity("Camera");
        registry.get<TransformComponent>(camera).local = glm::inverse(
            glm::lookAtRH(glm::vec3(190.0f, 190.0f, 400.0f), glm::vec3(190.0f, 190.0f, 0.0f), glm::vec3(0, 1, 0)));
        scene.markTransformDirty(camera);
        registry.emplace<CameraComponent>(camera);
        scene.markCameraDirty(camera);
        const Entity sun = scene.createEntity("Sun");
        registry.emplace<DirectionalLightComponent>(sun);
        scene.markLightDirty(sun);
        return {.success = true};
    }

    // Stats describe the frame submitted before this update.
    void update(ApplicationContext& context, const InputState&, float) override {
        ++m_frame;
        if (m_frame < 3) {
            return;
        }
        const RenderStats& stats = context.rendering.stats();
        const uint32_t frame = m_frame - 1;
        m_results.checks.check(stats.renderableCount <= objectCapacity, frame,
                               "at most the per-frame object capacity is drawn, not " +
                                   std::to_string(stats.renderableCount));
        m_results.checks.check(stats.droppedRenderItems >= extraCubes, frame,
                               "the renderables over the capacity are counted, not " +
                                   std::to_string(stats.droppedRenderItems));
        if (stats.particleEmitterCount == particles::maximumEmitters) {
            m_results.checks.check(stats.droppedParticleEmitters == extraEmitters, frame,
                                   "the emitters over the capacity are counted, not " +
                                       std::to_string(stats.droppedParticleEmitters));
        }
        ++m_results.checkedFrames;
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    SmokeResults& m_results;
    uint32_t m_frame{0};
};

class CapacityApplication final : public Application {
  public:
    explicit CapacityApplication(SmokeResults& results)
        : m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<CapacityScene>(m_results);
    }

  private:
    SmokeResults& m_results;
};

size_t warningsMentioning(const Logger& logger, const std::string& text) {
    const std::vector<LogEntry> entries = logger.entries();
    return static_cast<size_t>(std::ranges::count_if(entries, [&text](const LogEntry& entry) {
        return entry.level == LogLevel::Warning && entry.message.find(text) != std::string::npos;
    }));
}
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
    CapacityApplication application(results);
    const bool ran = engine.run(application);
    const size_t renderWarnings = warningsMentioning(engine.logger(), "Render items exceed");
    engine.shutdown();
    results.checks.check(ran, "the run ended early instead of rendering past the capacity");
    results.checks.check(results.checkedFrames != 0, "no frame was checked (is ZENITH_SMOKE_FRAMES at least 4?)");
    results.checks.check(renderWarnings == 1,
                         "the renderable overflow is logged once, not " + std::to_string(renderWarnings) + " times");
    const bool passed = results.checks.failures() == 0;
    if (passed) {
        std::cout << "Render capacity smoke test passed\n";
    }
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
