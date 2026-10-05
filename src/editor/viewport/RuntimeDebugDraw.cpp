#include "RuntimeDebugDraw.hpp"

#include "editor/theme/EditorPalette.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/navigation/NavAgent.hpp"
#include "engine/physics/CharacterController.hpp"
#include "engine/physics/PhysicsWorld.hpp"

#include <glm/glm.hpp>

namespace {
constexpr uint32_t segments = 16;
constexpr float contactMarkerRadius = 0.05f;
constexpr float contactNormalLength = 0.4f;
constexpr float groundNormalLength = 0.5f;

glm::vec4 debugColor(ThemeColor color) {
    return {color.r, color.g, color.b, color.a};
}

// Drawn over the scene: the overlays explain what is hidden inside or behind
// geometry (a capsule in a wall, an agent behind a crate).
DebugStyle onTop(ThemeColor color) {
    return {.color = debugColor(color), .depth = DebugDepth::AlwaysOnTop};
}

void drawContacts(const PhysicsWorld& physics, DebugDraw& output) {
    const DebugStyle style = onTop(palette::overlayContact);
    for (const ContactEvent& contact : physics.contactEvents()) {
        if (contact.type == ContactEventType::End) {
            continue;
        }
        output.sphere(contact.point, contactMarkerRadius, style, 8);
        output.arrow(contact.point, contact.point + contact.normal * contactNormalLength, style);
    }
}

void drawCharacters(const Scene& scene, DebugDraw& output) {
    for (const auto [entity, controller, transform] :
         scene.registry().view<const CharacterControllerComponent, const TransformComponent>().each()) {
        // The capsule is centred on the entity's origin and stands along
        // world Y (CharacterControllerComponent).
        const glm::vec3 center(transform.world[3]);
        const glm::vec3 spine(0.0f, controller.halfHeight, 0.0f);
        const DebugStyle style =
            onTop(controller.grounded ? palette::overlayCharacterGrounded : palette::overlayCharacterAirborne);
        output.capsule(center - spine, center + spine, controller.radius, style, segments);
        if (controller.grounded) {
            const glm::vec3 feet = center - glm::vec3(0.0f, controller.halfHeight + controller.radius, 0.0f);
            output.arrow(feet, feet + controller.groundNormal * groundNormalLength, style);
        }
    }
}

void drawNavAgents(const Scene& scene, const NavigationCrowd& crowd, DebugDraw& output) {
    for (const auto [entity, agent, transform] :
         scene.registry().view<const NavAgentComponent, const TransformComponent>().each()) {
        const glm::vec3 position(transform.world[3]);
        ThemeColor color = palette::overlayNavAgent;
        if (agent.pathFailed) {
            color = palette::overlayNavFailed;
        } else if (agent.partialPath) {
            color = palette::overlayNavPartial;
        }
        const bool inCrowd = crowd.owns(agent.crowdAgent, scene.id(entity).value);
        const NavCrowdAgentState state = inCrowd ? crowd.state(agent.crowdAgent) : NavCrowdAgentState{};
        if (inCrowd && state.partial && !agent.pathFailed) {
            color = palette::overlayNavPartial;
        }
        const DebugStyle style = onTop(color);
        output.circle(position, glm::vec3(0.0f, 1.0f, 0.0f), agent.radius, style, segments);
        if (!inCrowd) {
            continue;
        }
        if (glm::length(state.velocity) > 1e-3f) {
            output.arrow(position, position + state.velocity, style);
        }
        if (state.moving) {
            output.line(position, state.pathEnd, style);
            output.circle(state.pathEnd, glm::vec3(0.0f, 1.0f, 0.0f), agent.radius * 0.5f, style, segments);
        }
    }
}
} // namespace

void recordRuntimeDebug(const RuntimeDebugSources& sources, const RuntimeDebugOverlays& overlays, DebugDraw& output) {
    if (overlays.navigationMesh && sources.services != nullptr) {
        if (const NavigationWorld* navigation = sources.services->navigationIfBaked()) {
            navigation->drawDebug(output);
        }
    }
    if (sources.physics != nullptr) {
        if (overlays.physicsShapes) {
            // Fails only before init, when there is nothing to draw.
            sources.physics->drawDebug(output);
        }
        if (overlays.contacts) {
            drawContacts(*sources.physics, output);
        }
    }
    if (overlays.characters) {
        drawCharacters(sources.scene, output);
    }
    if (overlays.navAgents && sources.services != nullptr) {
        drawNavAgents(sources.scene, sources.services->navigationCrowd(), output);
    }
}
