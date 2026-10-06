#pragma once

// An entity that walks to a destination along the navigation mesh, steering around the other agents.

#include "NavigationCrowd.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>
#include <optional>

// An entity that walks to a destination along the navigation mesh, steering
// around the other agents. The engine's zenith.nav_agents system drives it
// through the session's NavigationCrowd: through the entity's
// CharacterControllerComponent when it has one (give it an empty move
// action, so player input does not drive it), otherwise by moving its
// transform directly. Speeds are metres per second, sizes metres.
//
// An agent stays in the crowd while it is idle, so others still walk around
// it.
struct NavAgentComponent {
    float speed{3.5f};
    // Closer than this to the destination, the agent has arrived.
    float stoppingDistance{0.2f};
    // How much room the agent takes among other agents.
    float radius{0.4f};

    // Runtime state, not serialized. Set the destination with
    // setNavDestination; it is cleared on arrival.
    std::optional<glm::vec3> destination;
    bool arrived{false};
    // The last destination was off the navigation mesh; set a new one to
    // retry.
    bool pathFailed{false};
    // The last destination cannot be reached, so the agent walks to (and
    // arrives at) the reachable point nearest it instead.
    bool partialPath{false};
    // The agent's slot in the session's crowd, and the destination it was
    // last sent to; managed by updateNavAgents.
    int crowdAgent{-1};
    std::optional<glm::vec3> requestedDestination;
};

// Sends the agent to destination; it plans on its next update.
void setNavDestination(NavAgentComponent& agent, const glm::vec3& destination);
// Stops the agent where it is.
void clearNavDestination(NavAgentComponent& agent);

// Moves every agent for deltaSeconds through crowd over navigation's mesh,
// first resetting the crowd (with settings) when the mesh was baked again
// since it was set up. Agents of entities that are gone leave the crowd.
void updateNavAgents(Scene& scene, const NavigationWorld& navigation, NavigationCrowd& crowd, float deltaSeconds,
                     const NavCrowdSettings& settings = {});
