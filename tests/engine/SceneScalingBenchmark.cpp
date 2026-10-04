// Scene costs that grow with entity count: a frame of physics step,
// interpolation and transform update over about 5k awake dynamic bodies, plus
// id lookups. Run by hand (Release) for numbers; CTest only checks that it
// completes.
#include "engine/diagnostics/Benchmark.hpp"
#include "engine/physics/PhysicsWorld.hpp"

#include <chrono>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t gridSide = 25;
constexpr uint32_t gridLayers = 8;
constexpr uint32_t bodyCount = gridSide * gridSide * gridLayers;
constexpr uint32_t warmupFrames = 10;
constexpr uint32_t measuredFrames = 60;
constexpr uint32_t lookupsPerFrame = 10'000;

void require(const PhysicsResult& result) {
    if (!result) {
        throw std::runtime_error(result.error);
    }
}

double millisecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

std::vector<SceneEntityId> populate(Scene& scene) {
    const Entity floor = scene.createEntity("Floor");
    scene.registry().get<TransformComponent>(floor).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(40.0f, 0.5f, 40.0f));
    scene.registry().emplace<ColliderComponent>(floor, BoxCollider{glm::vec3(1.0f)});
    scene.registry().emplace<RigidBodyComponent>(floor);

    std::vector<SceneEntityId> ids;
    ids.reserve(bodyCount);
    for (uint32_t index = 0; index < bodyCount; ++index) {
        const Entity entity = scene.createEntity("Benchmark body");
        const float x = static_cast<float>(index % gridSide) * 1.5f - 18.0f;
        const float z = static_cast<float>((index / gridSide) % gridSide) * 1.5f - 18.0f;
        // Spaced so the bodies are still falling, and so awake, while measured.
        const float y = 4.0f + static_cast<float>(index / (gridSide * gridSide)) * 3.0f;
        scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
        scene.registry().emplace<ColliderComponent>(entity, BoxCollider{glm::vec3(0.5f)});
        RigidBodyComponent body;
        body.motionType = PhysicsMotionType::Dynamic;
        body.layer = PhysicsLayer::Dynamic;
        scene.registry().emplace<RigidBodyComponent>(entity, body);
        ids.push_back(scene.id(entity));
    }
    return ids;
}
} // namespace

int main() {
    try {
        PhysicsWorldConfig config;
        config.maxBodies = 8192;
        config.maxBodyPairs = 65536;
        config.maxContactConstraints = 32768;
        config.temporaryAllocatorBytes = size_t{64} * 1024 * 1024;
        PhysicsWorld physics;
        require(physics.init(config));
        Scene scene;
        AssetManager assets;

        const auto populateBegin = std::chrono::steady_clock::now();
        const std::vector<SceneEntityId> ids = populate(scene);
        const double populateMilliseconds = millisecondsSince(populateBegin);

        BenchmarkReport report;
        addBuildMetadata(report);
        report.metadata("scenario", "scene-scaling-falling");
        report.metadata("bodies", std::to_string(bodyCount));
        report.metadata("measured_frames", std::to_string(measuredFrames));
        report.metadata("populate_ms", std::to_string(populateMilliseconds));

        const float fixedStep = physics.config().fixedTimeStep;
        size_t found = 0;
        for (uint32_t frame = 0; frame < warmupFrames + measuredFrames; ++frame) {
            const auto stepBegin = std::chrono::steady_clock::now();
            require(physics.step(scene, assets, fixedStep));
            const double stepMilliseconds = millisecondsSince(stepBegin);

            const auto interpolateBegin = std::chrono::steady_clock::now();
            physics.interpolate(scene, 0.5f);
            const double interpolateMilliseconds = millisecondsSince(interpolateBegin);

            const auto transformsBegin = std::chrono::steady_clock::now();
            scene.updateWorldTransforms();
            const double transformsMilliseconds = millisecondsSince(transformsBegin);
            const SceneTransformUpdateStats transformStats = scene.lastTransformUpdateStats();

            const auto lookupBegin = std::chrono::steady_clock::now();
            for (uint32_t lookup = 0; lookup < lookupsPerFrame; ++lookup) {
                // A fixed stride visits ids spread across the scene.
                found += scene.findEntity(ids[(lookup * 7919u + frame) % ids.size()]) != entt::null ? 1 : 0;
            }
            const double lookupMilliseconds = millisecondsSince(lookupBegin);

            if (frame < warmupFrames) {
                continue;
            }
            report.sample("cpu.physics_step", stepMilliseconds);
            report.sample("cpu.interpolate", interpolateMilliseconds);
            report.sample("cpu.transforms", transformsMilliseconds);
            report.sample("cpu.find_entity_10k", lookupMilliseconds);
            report.sample("transforms.visited", transformStats.visited, "count");
            report.sample("transforms.recomputed", transformStats.recomputed, "count");
            report.physics(physics.stats());
        }
        if (found != static_cast<size_t>(warmupFrames + measuredFrames) * lookupsPerFrame) {
            throw std::runtime_error("findEntity missed a live entity");
        }
        std::cout << report.json(false) << '\n';
        physics.shutdown();
    } catch (const std::exception& error) {
        std::cerr << "Scene scaling benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
