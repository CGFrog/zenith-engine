#include "NavAgent.hpp"

#include "NavigationWorld.hpp"
#include "engine/physics/CharacterController.hpp"

#include <algorithm>

namespace {
glm::vec3 horizontal(const glm::vec3& value) {
    return {value.x, 0.0f, value.z};
}

void stop(Scene& scene, Entity entity, NavAgentComponent& agent, bool arrived) {
    agent.destination.reset();
    agent.path.clear();
    agent.nextCorner = 0;
    agent.arrived = arrived;
    if (auto* character = scene.registry().try_get<CharacterControllerComponent>(entity)) {
        character->desiredVelocity = glm::vec3(0.0f);
    }
}
} // namespace

void setNavDestination(NavAgentComponent& agent, const glm::vec3& destination) {
    agent.destination = destination;
    agent.path.clear();
    agent.nextCorner = 0;
    agent.arrived = false;
    agent.pathFailed = false;
}

void clearNavDestination(NavAgentComponent& agent) {
    agent.destination.reset();
    agent.path.clear();
    agent.nextCorner = 0;
}

void updateNavAgents(Scene& scene, const NavigationWorld& navigation, float deltaSeconds) {
    entt::registry& registry = scene.registry();
    for (const Entity entity : registry.view<NavAgentComponent, TransformComponent>()) {
        auto& agent = registry.get<NavAgentComponent>(entity);
        if (!agent.destination) {
            continue;
        }
        auto& transform = registry.get<TransformComponent>(entity);
        const glm::vec3 position(transform.world[3]);
        if (agent.path.empty()) {
            if (!navigation.findPath(position, *agent.destination, agent.path)) {
                agent.pathFailed = true;
                stop(scene, entity, agent, false);
                continue;
            }
            // The first corner is where the agent stands.
            agent.nextCorner = agent.path.size() > 1 ? 1 : 0;
        }

        // Skip the corners the agent has reached.
        while (agent.nextCorner + 1 < agent.path.size() &&
               glm::length(horizontal(agent.path[agent.nextCorner] - position)) <= agent.stoppingDistance) {
            ++agent.nextCorner;
        }
        const glm::vec3 corner = agent.path[agent.nextCorner];
        const glm::vec3 offset = horizontal(corner - position);
        const float distance = glm::length(offset);
        const bool last = agent.nextCorner + 1 == agent.path.size();
        if (last && distance <= agent.stoppingDistance) {
            stop(scene, entity, agent, true);
            continue;
        }
        const glm::vec3 direction = distance > 1.0e-5f ? offset / distance : glm::vec3(0.0f);

        if (auto* character = registry.try_get<CharacterControllerComponent>(entity)) {
            character->desiredVelocity = direction * agent.speed;
            continue;
        }
        // Without a character, move along the path, following its height.
        const float step = std::min(distance, agent.speed * std::max(deltaSeconds, 0.0f));
        glm::vec3 moved = position + direction * step;
        moved.y = distance > 1.0e-5f ? position.y + (corner.y - position.y) * (step / distance) : corner.y;
        glm::mat4 world = transform.world;
        world[3] = glm::vec4(moved, 1.0f);
        const Entity parent = registry.get<RelationshipComponent>(entity).parent;
        transform.local = parent != entt::null && registry.valid(parent)
                              ? glm::inverse(registry.get<TransformComponent>(parent).world) * world
                              : world;
        scene.markTransformDirty(entity);
    }
}
