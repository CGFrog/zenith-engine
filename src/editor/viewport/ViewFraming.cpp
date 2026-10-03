#include "ViewFraming.hpp"

#include <algorithm>
#include <limits>

void frameEntities(EditorCamera& camera, Scene& scene, const AssetManager& assets,
                   std::span<const SceneEntityId> entities) {
    scene.updateWorldTransforms();
    constexpr float objectRadius = 0.5f;
    glm::vec3 lower(std::numeric_limits<float>::max());
    glm::vec3 upper(std::numeric_limits<float>::lowest());
    bool any = false;
    for (SceneEntityId id : entities) {
        const Entity entity = scene.findEntity(id);
        if (entity == entt::null) {
            continue;
        }
        const glm::mat4& world = scene.registry().get<TransformComponent>(entity).world;
        glm::vec3 center(world[3]);
        float radius = objectRadius;
        if (const auto* renderer = scene.registry().try_get<MeshRendererComponent>(entity)) {
            if (const MeshAsset* mesh = assets.mesh(renderer->mesh)) {
                const float scale = std::max({glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                                              glm::length(glm::vec3(world[2]))});
                center = glm::vec3(world * glm::vec4(mesh->bounds.center, 1.0f));
                radius = mesh->bounds.radius * scale;
            }
        }
        lower = glm::min(lower, center - radius);
        upper = glm::max(upper, center + radius);
        any = true;
    }
    if (any) {
        camera.focusOnBounds((lower + upper) * 0.5f, std::max(glm::length(upper - lower) * 0.5f, objectRadius));
    }
}
