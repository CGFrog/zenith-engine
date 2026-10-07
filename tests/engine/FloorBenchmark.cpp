// What building and replacing a roguelike floor costs: spawning 40 room
// prefabs (six static box colliders, a light, and a looping sound each)
// through the session's commands, the physics step that creates their bodies,
// the navigation bake, and destroying the floor again. Run by hand (Release)
// for numbers; CTest only checks that it completes.

#include "FloorFixture.hpp"

#include "engine/diagnostics/Benchmark.hpp"

#include <chrono>
#include <iostream>

namespace {
constexpr uint32_t roomCount = 40;
constexpr uint32_t columns = 8;
constexpr uint32_t cycles = 5;

double millisecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

template <typename Body> void timed(BenchmarkReport& report, const char* name, Body&& body) {
    const auto begin = std::chrono::steady_clock::now();
    body();
    report.sample(name, millisecondsSince(begin));
}
} // namespace

int main() {
    using namespace floor_fixture;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_floor_benchmark";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    BenchmarkReport report;
    try {
        FloorSession session(root);
        session.step();
        for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
            timed(report, "floor.spawn", [&] { session.spawnFloor(roomCount, columns); });
            timed(report, "floor.first_step", [&] { session.step(); });
            timed(report, "floor.navigation_bake",
                  [&] { require(session.services->outside().bakeNavigation(session.scene), "bake the floor"); });
            timed(report, "floor.destroy", [&] { session.destroyFloor(); });
            timed(report, "floor.teardown_step", [&] { session.step(); });
        }
        const NavMeshStats& mesh = session.services->outside().navigation(session.scene).stats();
        std::cout << roomCount << " rooms, " << roomCount * bodiesPerRoom << " bodies; last navigation mesh "
                  << mesh.polygons << " polygons from " << mesh.inputTriangles << " triangles\n";
        session.audio.shutdown();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::filesystem::remove_all(root);
    std::cout << report.json() << '\n';
    return 0;
}
