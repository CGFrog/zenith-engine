// Building and replacing a floor at runtime, as a roguelike does between
// levels: room prefabs spawned through the session's commands get physics
// bodies, sounding emitters, and a navigation mesh baked on demand, and
// destroying the rooms releases every body and voice while the player stays.

#include "FloorFixture.hpp"

#include "support/Doctest.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {
using namespace floor_fixture;
} // namespace

TEST_SUITE_BEGIN("FloorLifecycle");

TEST_CASE("FloorLifecycle: floor cycles") {
    const testsupport::TempDirectory root("floor-lifecycle-cycles");
    FloorSession session(root.path());
    session.step();
    const uint32_t persistentBodies = session.physics.stats().bodies;
    CHECK_MESSAGE(persistentBodies == 1, "the player is the only body before a floor exists");

    constexpr uint32_t rooms = 6;
    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
        const std::string name = "cycle " + std::to_string(cycle) + ": ";
        session.spawnFloor(rooms, 3);
        // Baked straight after the spawn, before anything updates world
        // transforms: the rooms' placements are still only local.
        const NavigationResult baked = session.services->outside().bakeNavigation(session.scene);
        session.step();
        CHECK_MESSAGE(session.rooms.size() == rooms, (name + "every room spawns"));
        CHECK_MESSAGE(session.physics.stats().bodies == persistentBodies + rooms * bodiesPerRoom,
                      (name + "every room's colliders become bodies"));
        CHECK_MESSAGE(session.activeVoices() == rooms, (name + "every room's torch sounds"));

        CHECK_MESSAGE((baked.success && session.services->outside().navigation(session.scene).baked()),
                      (name + "the floor's navigation mesh bakes on demand: " + baked.error));
        // The rooms have no doorways, so the mesh is one island per room.
        const NavigationWorld& navigation = session.services->outside().navigation(session.scene);
        const glm::vec3 farthestRoom(2.0f * roomSize - 3.0f, 0.0f, roomSize - 3.0f);
        const std::optional<glm::vec3> onMesh = navigation.nearestPoint(farthestRoom);
        std::vector<glm::vec3> path;
        CHECK_MESSAGE(
            (onMesh && glm::distance(*onMesh, farthestRoom) < 0.5f &&
             navigation.findPath(farthestRoom, farthestRoom + glm::vec3(1.0f, 0.0f, 4.0f), path) == NavPath::Complete),
            (name + "the baked mesh covers the farthest room"));

        session.destroyFloor();
        session.step();
        CHECK_MESSAGE(session.physics.stats().bodies == persistentBodies,
                      (name + "replacing the floor removes its bodies"));
        CHECK_MESSAGE(session.activeVoices() == 0, (name + "replacing the floor stops its sounds"));
        CHECK_MESSAGE((session.scene.findEntity(session.player) != entt::null),
                      (name + "the player survives the floor"));
    }

    // With the rooms gone, nothing walkable is left to bake.
    CHECK_MESSAGE((!session.services->outside().bakeNavigation(session.scene).success &&
                   !session.services->outside().navigation(session.scene).baked()),
                  "a bake with no floor fails and leaves no mesh");
    session.audio.shutdown();
}

TEST_SUITE_END();
