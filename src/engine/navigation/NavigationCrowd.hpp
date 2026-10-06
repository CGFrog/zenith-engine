#pragma once

// How a crowd steers its agents around each other.

#include "NavigationWorld.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

// How a crowd steers its agents around each other.
struct NavCrowdSettings {
    // Agents beyond this many are not admitted (addAgent fails).
    uint32_t maximumAgents{128};
    // The largest agent radius the crowd plans for, in metres; larger agents
    // are clamped to it.
    float maximumAgentRadius{1.0f};
    // How strongly agents keep apart; 0 turns separation off (they still
    // avoid each other's paths).
    float separationWeight{2.0f};
};

// One agent's size and pace.
struct NavCrowdAgentParams {
    float radius{0.4f};
    float height{1.8f};
    float maximumSpeed{3.5f};
};

// Where a crowd agent stands and is heading after an update.
struct NavCrowdAgentState {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    // Where its current path ends: the target, or the reachable point
    // nearest it when the path is partial.
    glm::vec3 pathEnd{0.0f};
    // The target cannot be reached; the path ends as near it as the mesh
    // allows.
    bool partial{false};
    // A move target is set and its path is planned (not still being found).
    bool moving{false};
};

// Agents moving over one NavigationWorld's mesh together, steering around
// each other (Detour's dtCrowd, which stays private). Agents are indices;
// each records the owner it was added for (an entity id), so a caller can
// tell a slot it holds from one reused for someone else.
//
// The crowd refers to the world's mesh: reset it after the mesh is baked
// again (NavigationWorld::generation changes); resetting removes every
// agent.
class NavigationCrowd {
  public:
    NavigationCrowd();
    ~NavigationCrowd();

    NavigationCrowd(const NavigationCrowd&) = delete;
    NavigationCrowd& operator=(const NavigationCrowd&) = delete;

    // Starts over on navigation's mesh, removing every agent. Fails, leaving
    // no crowd, when the mesh is not baked.
    NavigationResult reset(const NavigationWorld& navigation, const NavCrowdSettings& settings = {});
    bool ready() const;
    // The world generation the crowd was reset for.
    uint64_t meshGeneration() const;

    // Adds an agent at the point of the mesh nearest position; -1 when the
    // crowd is full or position is off the mesh.
    int addAgent(const glm::vec3& position, const NavCrowdAgentParams& params, uint64_t owner);
    void removeAgent(int agent);
    // Whether agent is an active slot added for owner.
    bool owns(int agent, uint64_t owner) const;
    // Sends the agent toward the point of the mesh nearest target; false
    // when target is off the mesh.
    bool requestMove(int agent, const glm::vec3& target);
    // Stops the agent where it is; it still keeps others away.
    void stop(int agent);
    // Moves the agent's crowd position to where it really is (a character
    // the physics moved), before the next update.
    void placeAgent(int agent, const glm::vec3& position);

    // Plans and steers every agent for deltaSeconds.
    void update(float deltaSeconds);
    NavCrowdAgentState state(int agent) const;

    // Agents not marked since the last sweep are removed: a caller marks the
    // agents it still holds each update, so agents whose owners are gone
    // (destroyed entities) leave the crowd.
    void mark(int agent);
    void sweepUnmarked();
    size_t agentCount() const;

  private:
    bool valid(int agent) const;

    class Impl;
    std::unique_ptr<Impl> m_impl;
};
