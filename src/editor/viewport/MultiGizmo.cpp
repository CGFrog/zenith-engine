#include "MultiGizmo.hpp"

#include <algorithm>

glm::mat4 gizmoFrame(std::span<const glm::mat4> startWorld, const GizmoPlacement& placement) {
    ModalTransformPivot pivot = placement.pivot;
    if (placement.translating && pivot.mode == TransformPivot::Cursor) {
        pivot.mode = TransformPivot::MedianPoint;
    }
    glm::mat4 frame(placement.orientation);
    frame[3] = glm::vec4(transformPivotPoint(pivot, startWorld), 1.0f);
    return frame;
}

std::vector<glm::mat4> applyGizmoChange(std::span<const glm::mat4> startWorld, TransformPivot pivotMode,
                                        const glm::mat4& gizmoStart, const glm::mat4& gizmoNow) {
    const std::optional<PivotedTransform> change = decomposeGizmoChange(gizmoStart, gizmoNow);
    if (!change) {
        return {startWorld.begin(), startWorld.end()};
    }
    return applyPivotedTransform(startWorld, pivotMode, glm::vec3(gizmoStart[3]), normalizedAxes(gizmoStart), *change);
}

void setWorldTransforms(Scene& scene, std::span<const Entity> entities, std::span<const glm::mat4> world) {
    entt::registry& registry = scene.registry();
    const size_t count = std::min(entities.size(), world.size());
    for (size_t index = 0; index < count; ++index) {
        const Entity entity = entities[index];
        if (!registry.valid(entity)) {
            continue;
        }
        const Entity parent = registry.get<RelationshipComponent>(entity).parent;
        registry.get<TransformComponent>(entity).local =
            parent == entt::null ? world[index]
                                 : glm::inverse(registry.get<TransformComponent>(parent).world) * world[index];
        scene.markTransformDirty(entity);
    }
    scene.updateWorldTransforms();
}
