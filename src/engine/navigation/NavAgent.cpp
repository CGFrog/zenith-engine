#include "NavAgent.hpp"

#include "NavigationCrowd.hpp"
#include "NavigationWorld.hpp"
#include "engine/physics/CharacterController.hpp"

#include <algorithm>

namespace {
glm::vec3 horizontal(const glm::vec3& value) {
    return {value.x, 0.0f, value.z};
}

void halt(Scene& scene, Entity entity, NavAgentComponent& agent, NavigationCrowd& crowd, bool arrived) {
    agent.destination.reset();
    agent.requestedDestination.reset();
    agent.arrived = arrived;
    crowd.stop(agent.crowdAgent);
    if (auto* character = scene.registry().try_get<CharacterControllerComponent>(entity)) {
        character->desiredVelocity = glm::vec3(0.0f);
    }
}

// The agent's crowd slot, joining the crowd when it has none (or its slot
// was given to someone else, as a duplicated entity's copied slot is).
bool joinCrowd(NavAgentComponent& agent, NavigationCrowd& crowd, const NavigationWorld& navigation,
               const glm::vec3& position, uint64_t owner) {
    if (crowd.owns(agent.crowdAgent, owner)) {
        crowd.mark(agent.crowdAgent);
        return true;
    }
    agent.requestedDestination.reset();
    agent.crowdAgent = crowd.addAgent(
        position, {.radius = agent.radius, .height = navigation.settings().agentHeight, .maximumSpeed = agent.speed},
        owner);
    return agent.crowdAgent >= 0;
}

// Places the entity where the crowd moved it, keeping its parent.
void moveTransform(Scene& scene, Entity entity, const glm::vec3& position) {
    entt::registry& registry = scene.registry();
    auto& transform = registry.get<TransformComponent>(entity);
    glm::mat4 world = transform.world;
    world[3] = glm::vec4(position, 1.0f);
    const Entity parent = registry.get<RelationshipComponent>(entity).parent;
    transform.local = parent != entt::null && registry.valid(parent)
                          ? glm::inverse(registry.get<TransformComponent>(parent).world) * world
                          : world;
    scene.markTransformDirty(entity);
}
} // namespace

void setNavDestination(NavAgentComponent& agent, const glm::vec3& destination) {
    agent.destination = destination;
    agent.arrived = false;
    agent.pathFailed = false;
    agent.partialPath = false;
}

void clearNavDestination(NavAgentComponent& agent) {
    agent.destination.reset();
}

void updateNavAgents(Scene& scene, const NavigationWorld& navigation, NavigationCrowd& crowd, float deltaSeconds,
                     const NavCrowdSettings& settings) {
    entt::registry& registry = scene.registry();
    if (crowd.meshGeneration() != navigation.generation() || !crowd.ready()) {
        // A new mesh: every agent joins the new crowd.
        if (!navigation.baked() || !crowd.reset(navigation, settings)) {
            return;
        }
    }

    // Before the crowd steps: join, aim, and place agents where they stand.
    for (const Entity entity : registry.view<NavAgentComponent, TransformComponent>()) {
        auto& agent = registry.get<NavAgentComponent>(entity);
        const glm::vec3 position(registry.get<TransformComponent>(entity).world[3]);
        if (!joinCrowd(agent, crowd, navigation, position, scene.id(entity).value)) {
            if (agent.destination) {
                agent.pathFailed = true;
                halt(scene, entity, agent, crowd, false);
            }
            continue;
        }
        // Physics moves characters, and a game may teleport any agent.
        crowd.placeAgent(agent.crowdAgent, position);
        if (!agent.destination) {
            if (agent.requestedDestination) {
                halt(scene, entity, agent, crowd, agent.arrived);
            }
            continue;
        }
        if (agent.requestedDestination != agent.destination) {
            if (!crowd.requestMove(agent.crowdAgent, *agent.destination)) {
                agent.pathFailed = true;
                halt(scene, entity, agent, crowd, false);
                continue;
            }
            agent.requestedDestination = agent.destination;
        }
    }
    crowd.sweepUnmarked();
    crowd.update(deltaSeconds);

    // After: follow the crowd, and stop at the end of the path.
    for (const Entity entity : registry.view<NavAgentComponent, TransformComponent>()) {
        auto& agent = registry.get<NavAgentComponent>(entity);
        if (!agent.destination || !agent.requestedDestination) {
            continue;
        }
        const NavCrowdAgentState state = crowd.state(agent.crowdAgent);
        if (!state.moving) {
            continue;
        }
        agent.partialPath = state.partial;
        const glm::vec3 position(registry.get<TransformComponent>(entity).world[3]);
        if (glm::length(horizontal(state.pathEnd - position)) <= agent.stoppingDistance) {
            halt(scene, entity, agent, crowd, true);
            continue;
        }
        if (auto* character = registry.try_get<CharacterControllerComponent>(entity)) {
            character->desiredVelocity = horizontal(state.velocity);
        } else {
            moveTransform(scene, entity, state.position);
        }
    }
}
