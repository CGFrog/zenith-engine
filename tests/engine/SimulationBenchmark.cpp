#include "engine/diagnostics/Benchmark.hpp"
#include "engine/physics/PhysicsWorld.hpp"

#include <chrono>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr uint32_t bodyCount = 256;
constexpr uint32_t warmupSteps = 300;
constexpr uint32_t measuredSteps = 300;

void require(const PhysicsResult& result) {
    if (!result) {
        throw std::runtime_error(result.error);
    }
}

void runScenario(std::string_view scenario, bool activateBodies) {
    PhysicsWorld physics;
    require(physics.init());
    Scene scene;
    AssetManager assets;

    const Entity floor = scene.createEntity("Floor");
    scene.registry().get<TransformComponent>(floor).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(20.0f, 0.5f, 20.0f));
    scene.registry().emplace<ColliderComponent>(floor, BoxCollider{glm::vec3(1.0f)});
    scene.registry().emplace<RigidBodyComponent>(floor);

    std::vector<Entity> bodies;
    bodies.reserve(bodyCount);
    for (uint32_t index = 0; index < bodyCount; ++index) {
        const Entity entity = scene.createEntity("Benchmark body");
        const float x = static_cast<float>(index % 8) * 1.25f - 4.375f;
        const float z = static_cast<float>((index / 8) % 8) * 1.25f - 4.375f;
        const float y = 0.55f + static_cast<float>(index / 64) * 1.1f;
        scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
        scene.registry().emplace<ColliderComponent>(entity, BoxCollider{glm::vec3(0.5f)});
        RigidBodyComponent body;
        body.motionType = PhysicsMotionType::Dynamic;
        body.layer = PhysicsLayer::Dynamic;
        scene.registry().emplace<RigidBodyComponent>(entity, body);
        bodies.push_back(entity);
    }

    for (uint32_t step = 0; step < warmupSteps; ++step) {
        require(physics.step(scene, assets, physics.config().fixedTimeStep));
    }
    if (activateBodies) {
        for (Entity entity : bodies) {
            if (!physics.applyImpulse(entity, {1.5f, 3.0f, 0.75f})) {
                throw std::runtime_error("Could not activate benchmark body");
            }
        }
    }

    BenchmarkReport report;
    addBuildMetadata(report);
    report.metadata("scenario", std::string(scenario));
    report.metadata("bodies", std::to_string(bodyCount));
    report.metadata("warmup_steps", std::to_string(warmupSteps));
    report.metadata("measured_steps", std::to_string(measuredSteps));
    for (uint32_t step = 0; step < measuredSteps; ++step) {
        const auto begin = std::chrono::steady_clock::now();
        require(physics.step(scene, assets, physics.config().fixedTimeStep));
        report.sample("cpu.physics_step",
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
        report.physics(physics.stats());
    }
    std::cout << report.json(false) << '\n';
    physics.shutdown();
}
} // namespace

int main() {
    try {
        runScenario("physics-sleeping", false);
        runScenario("physics-active", true);
    } catch (const std::exception& error) {
        std::cerr << "Simulation benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
