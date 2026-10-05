// Building and replacing a floor at runtime, as a roguelike does between
// levels: room prefabs spawned through the session's commands get physics
// bodies, sounding emitters, and a navigation mesh baked on demand, and
// destroying the rooms releases every body and voice while the player stays.

#include "FloorFixture.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

using namespace floor_fixture;

void testFloorCycles() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_floor_lifecycle_tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        FloorSession session(root);
        session.step();
        const uint32_t persistentBodies = session.physics.stats().bodies;
        expect(persistentBodies == 1, "the player is the only body before a floor exists");

        constexpr uint32_t rooms = 6;
        for (uint32_t cycle = 0; cycle < 3; ++cycle) {
            const std::string name = "cycle " + std::to_string(cycle) + ": ";
            session.spawnFloor(rooms, 3);
            // Baked straight after the spawn, before anything updates world
            // transforms: the rooms' placements are still only local.
            const NavigationResult baked = session.services->bakeNavigation(session.scene);
            session.step();
            expect(session.rooms.size() == rooms, name + "every room spawns");
            expect(session.physics.stats().bodies == persistentBodies + rooms * bodiesPerRoom,
                   name + "every room's colliders become bodies");
            expect(session.activeVoices() == rooms, name + "every room's torch sounds");

            expect(baked.success && session.services->navigation(session.scene).baked(),
                   name + "the floor's navigation mesh bakes on demand: " + baked.error);
            // The rooms have no doorways, so the mesh is one island per room.
            const NavigationWorld& navigation = session.services->navigation(session.scene);
            const glm::vec3 farthestRoom(2.0f * roomSize - 3.0f, 0.0f, roomSize - 3.0f);
            const std::optional<glm::vec3> onMesh = navigation.nearestPoint(farthestRoom);
            std::vector<glm::vec3> path;
            expect(onMesh && glm::distance(*onMesh, farthestRoom) < 0.5f &&
                       navigation.findPath(farthestRoom, farthestRoom + glm::vec3(1.0f, 0.0f, 4.0f), path) ==
                           NavPath::Complete,
                   name + "the baked mesh covers the farthest room");

            session.destroyFloor();
            session.step();
            expect(session.physics.stats().bodies == persistentBodies, name + "replacing the floor removes its bodies");
            expect(session.activeVoices() == 0, name + "replacing the floor stops its sounds");
            expect(session.scene.findEntity(session.player) != entt::null, name + "the player survives the floor");
        }

        // With the rooms gone, nothing walkable is left to bake.
        expect(!session.services->bakeNavigation(session.scene).success &&
                   !session.services->navigation(session.scene).baked(),
               "a bake with no floor fails and leaves no mesh");
        session.audio.shutdown();
    }
    std::filesystem::remove_all(root);
}
} // namespace

int main() {
    try {
        testFloorCycles();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0) {
        std::cout << "Floor lifecycle tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
