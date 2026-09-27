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
    constexpr int iterations = 200;
    volatile uint64_t sink = 0;
    const auto uncachedStart = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        std::vector<uint64_t> packets;
        packets.reserve(2000);
        auto view = scene.registry().view<const TransformComponent, const MeshRendererComponent>();
        for (Entity entity : view) {
            const auto& mesh = view.get<const MeshRendererComponent>(entity).mesh;
            packets.push_back((static_cast<uint64_t>(mesh.generation) << 32) | mesh.index);
        }
        std::ranges::sort(packets);
        sink = sink ^ packets.front();
    }
    const auto uncachedEnd = std::chrono::steady_clock::now();
    const auto cachedStart = std::chrono::steady_clock::now();
    uint64_t revision = scene.renderPacketRevision();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const uint64_t current = scene.renderPacketRevision();
        if (current != revision) {
            revision = current;
        }
        sink = sink ^ revision;
    }
    const auto cachedEnd = std::chrono::steady_clock::now();
    const double uncached = std::chrono::duration<double, std::milli>(uncachedEnd - uncachedStart).count();
    const double cached = std::chrono::duration<double, std::milli>(cachedEnd - cachedStart).count();
    std::cout << "{\"objects\":2000,\"iterations\":" << iterations << ",\"uncached_ms\":" << uncached
              << ",\"cached_ms\":" << cached << ",\"sink\":" << sink << "}\n";
    return cached < uncached ? 0 : 1;
}
