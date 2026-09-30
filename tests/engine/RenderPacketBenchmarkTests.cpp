#include "engine/diagnostics/Benchmark.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

int main() {
    Scene scene;
    for (uint32_t index = 0; index < 2000; ++index) {
        const Entity entity = scene.createEntity();
        scene.attachMesh(entity, {.index = index, .generation = 1});
        scene.registry().get<TransformComponent>(entity).local[3].x = static_cast<float>(index % 100);
    }
    scene.updateWorldTransforms();
    constexpr int warmupIterations = 20;
    constexpr int iterations = 200;
    volatile uint64_t sink = 0;
    BenchmarkReport report;
    addBuildMetadata(report);
    report.metadata("scenario", "render-packet-cache");
    report.metadata("objects", "2000");
    report.metadata("warmup_iterations", std::to_string(warmupIterations));
    report.metadata("measured_iterations", std::to_string(iterations));
    for (int iteration = -warmupIterations; iteration < iterations; ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        std::vector<uint64_t> packets;
        packets.reserve(2000);
        auto view = scene.registry().view<const TransformComponent, const MeshRendererComponent>();
        for (Entity entity : view) {
            const auto& mesh = view.get<const MeshRendererComponent>(entity).mesh;
            packets.push_back((static_cast<uint64_t>(mesh.generation) << 32) | mesh.index);
        }
        std::sort(packets.begin(), packets.end());
        sink = sink ^ packets.front();
        if (iteration >= 0) {
            report.sample("cpu.packet_rebuild",
                          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count(),
                          "us");
        }
    }
    uint64_t revision = scene.renderPacketRevision();
    for (int iteration = -warmupIterations; iteration < iterations; ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        const uint64_t current = scene.renderPacketRevision();
        if (current != revision) {
            revision = current;
        }
        sink = sink ^ revision;
        if (iteration >= 0) {
            report.sample("cpu.packet_cache_check",
                          std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count(),
                          "ns");
        }
    }
    report.metadata("correctness_sink", std::to_string(sink));
    std::cout << report.json(false) << '\n';
    return 0;
}
