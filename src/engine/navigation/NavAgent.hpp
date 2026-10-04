#pragma once

#include "engine/scene/Scene.hpp"

#include <cstddef>
#include <glm/glm.hpp>
#include <optional>
#include <vector>

class NavigationWorld;

// An entity that walks to a destination along the navigation mesh. The
// engine's zenith.nav_agents system plans a path when the destination changes
// and steers along it: through the entity's CharacterControllerComponent when
// it has one (give it an empty move action, so player input does not drive
// it), otherwise by moving its transform directly. Speeds are metres per
// second.
struct NavAgentComponent {
    float speed{3.5f};
    // Closer than this to the destination, the agent has arrived.
    float stoppingDistance{0.2f};

    // Runtime state, not serialized. Set the destination with
    // setNavDestination; it is cleared on arrival.
    std::optional<glm::vec3> destination;
    std::vector<glm::vec3> path;
    size_t nextCorner{0};
    bool arrived{false};
    // The last destination could not be reached; set a new one to retry.
    bool pathFailed{false};
};

// Sends the agent to destination, planning a new path on its next update.
void setNavDestination(NavAgentComponent& agent, const glm::vec3& destination);
void clearNavDestination(NavAgentComponent& agent);

// Plans and follows every agent's path for deltaSeconds.
void updateNavAgents(Scene& scene, const NavigationWorld& navigation, float deltaSeconds);
