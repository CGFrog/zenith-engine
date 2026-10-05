#include "ColliderHandles.hpp"

#include "editor/interaction/EditorCamera.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorPalette.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/PixelPainter.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <variant>

namespace {
constexpr float minimumColliderSize = 0.001f;

// The shape's frame in the world: its centre and rotation, and the entity's
// absolute world scale, as physics splits the entity's world matrix (a
// mirrored matrix negates all three axes) and then places the shape
// (colliderLocalTransform).
struct ShapeFrame {
    glm::vec3 center{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

ShapeFrame shapeFrame(const ColliderComponent& collider, const glm::mat4& entityWorld) {
    ShapeFrame frame;
    glm::mat3 axes(entityWorld);
    for (int axis = 0; axis < 3; ++axis) {
        frame.scale[axis] = glm::length(axes[axis]);
        axes[axis] = frame.scale[axis] > 1e-12f ? axes[axis] / frame.scale[axis] : glm::vec3(0.0f);
    }
    if (glm::determinant(axes) < 0.0f) {
        axes = -axes;
    }
    const glm::quat entityRotation = glm::normalize(glm::quat_cast(axes));
    const ColliderPlacement placement = colliderLocalTransform(collider, frame.scale);
    frame.center = glm::vec3(entityWorld[3]) + entityRotation * placement.position;
    frame.rotation = glm::normalize(entityRotation * placement.rotation);
    return frame;
}

glm::vec3 unitAxis(int axis, float side) {
    glm::vec3 direction(0.0f);
    direction[axis] = side;
    return direction;
}

// How much one unit of the dimension measures in the world on an entity of
// this scale, as PhysicsWorld sizes the shape.
float dimensionScale(const ColliderComponent& collider, const ColliderHandle& handle, const glm::vec3& scale) {
    switch (handle.dimension) {
    case ColliderHandle::Dimension::BoxHalfExtent:
        return scale[handle.axis];
    case ColliderHandle::Dimension::Radius:
        return std::holds_alternative<SphereCollider>(collider.shape) ? std::max({scale.x, scale.y, scale.z})
                                                                      : std::max(scale.x, scale.z);
    case ColliderHandle::Dimension::CapsuleHalfHeight:
        return scale.y;
    }
    return 1.0f;
}

// The dimension the handle resizes, or null when the shape has no such
// dimension (it changed since the handles were found).
float* dimension(ColliderComponent& collider, const ColliderHandle& handle) {
    switch (handle.dimension) {
    case ColliderHandle::Dimension::BoxHalfExtent:
        if (auto* box = std::get_if<BoxCollider>(&collider.shape)) {
            return &box->halfExtents[handle.axis];
        }
        return nullptr;
    case ColliderHandle::Dimension::Radius:
        if (auto* sphere = std::get_if<SphereCollider>(&collider.shape)) {
            return &sphere->radius;
        }
        if (auto* capsule = std::get_if<CapsuleCollider>(&collider.shape)) {
            return &capsule->radius;
        }
        return nullptr;
    case ColliderHandle::Dimension::CapsuleHalfHeight:
        if (auto* capsule = std::get_if<CapsuleCollider>(&collider.shape)) {
            return &capsule->halfHeight;
        }
        return nullptr;
    }
    return nullptr;
}

// A handle's view pixel position, or nullopt behind the lens.
std::optional<glm::vec2> project(const glm::mat4& viewProjection, glm::vec2 viewSize, const glm::vec3& point) {
    const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
    if (clip.w <= 1e-6f) {
        return std::nullopt;
    }
    // Clip space is Y-down (lensProjectionMatrix), as view pixels are.
    return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * viewSize;
}
} // namespace

std::vector<ColliderHandle> colliderHandles(const ColliderComponent& collider, const glm::mat4& entityWorld) {
    const ShapeFrame frame = shapeFrame(collider, entityWorld);
    std::vector<ColliderHandle> handles;
    const auto add = [&](ColliderHandle::Dimension kind, int axis, float side, float distance) {
        const glm::vec3 direction = frame.rotation * unitAxis(axis, side);
        handles.push_back({.dimension = kind,
                           .axis = axis,
                           .side = side,
                           .position = frame.center + direction * distance,
                           .direction = direction});
    };
    const glm::vec3& scale = frame.scale;
    if (const auto* box = std::get_if<BoxCollider>(&collider.shape)) {
        for (int axis = 0; axis < 3; ++axis) {
            for (const float side : {1.0f, -1.0f}) {
                add(ColliderHandle::Dimension::BoxHalfExtent, axis, side, box->halfExtents[axis] * scale[axis]);
            }
        }
    } else if (const auto* sphere = std::get_if<SphereCollider>(&collider.shape)) {
        const float radius = sphere->radius * std::max({scale.x, scale.y, scale.z});
        for (int axis = 0; axis < 3; ++axis) {
            for (const float side : {1.0f, -1.0f}) {
                add(ColliderHandle::Dimension::Radius, axis, side, radius);
            }
        }
    } else if (const auto* capsule = std::get_if<CapsuleCollider>(&collider.shape)) {
        // Jolt capsules run along their local Y axis.
        const float radius = capsule->radius * std::max(scale.x, scale.z);
        for (const int axis : {0, 2}) {
            for (const float side : {1.0f, -1.0f}) {
                add(ColliderHandle::Dimension::Radius, axis, side, radius);
            }
        }
        for (const float side : {1.0f, -1.0f}) {
            add(ColliderHandle::Dimension::CapsuleHalfHeight, 1, side, capsule->halfHeight * scale.y + radius);
        }
    }
    return handles;
}

ColliderComponent dragColliderHandle(const ColliderComponent& start, const ColliderHandle& handle,
                                     const glm::vec3& worldScale, float distance, ColliderHandleDrag mode) {
    ColliderComponent dragged = start;
    float* value = dimension(dragged, handle);
    const float unit = dimensionScale(start, handle, worldScale);
    if (value == nullptr || !(unit > 1e-6f) || !std::isfinite(distance)) {
        return dragged;
    }
    // A symmetric drag moves both sides, so the half size grows by the whole
    // distance; a one-sided drag spreads it over the full size.
    const float requested = mode == ColliderHandleDrag::Symmetric ? distance : distance * 0.5f;
    const float before = *value;
    *value = std::max(before + requested / unit, minimumColliderSize);
    if (mode == ColliderHandleDrag::OneSided) {
        // The centre follows the dragged side by as much as the half size
        // grew, in the entity's local axes before its scale.
        const float grown = (*value - before) * unit;
        const glm::vec3 shift = start.offsetRotation * unitAxis(handle.axis, handle.side) * grown;
        dragged.offsetPosition += shift / glm::max(worldScale, glm::vec3(1e-6f));
    }
    return dragged;
}

std::optional<float> colliderHandleAxisParameter(const RenderView& lens, glm::vec2 viewSize, glm::vec2 mouse,
                                                 const glm::vec3& origin, const glm::vec3& direction) {
    if (viewSize.x <= 0.0f || viewSize.y <= 0.0f) {
        return std::nullopt;
    }
    const LensRay ray = lensRay(lens, mouse / viewSize, viewSize.x / viewSize.y);
    // The closest points of the handle's line and the mouse ray.
    const float alignment = glm::dot(direction, ray.direction);
    const float denominator = 1.0f - alignment * alignment;
    if (denominator < 1e-4f) {
        return std::nullopt;
    }
    const glm::vec3 between = origin - ray.origin;
    return (alignment * glm::dot(ray.direction, between) - glm::dot(direction, between)) / denominator;
}

bool ColliderHandleTool::update(EditorContext& context, const RenderView& lens, glm::vec2 viewSize,
                                const ColliderHandleInput& input, ImDrawList* drawList, glm::vec2 viewPosition) {
    m_hovered.reset();
    Scene& scene = context.app.scene;
    const std::optional<SceneEntityId> active = context.selection.active();
    const Entity entity = active ? scene.findEntity(*active) : entt::null;
    if (m_drag &&
        (entity == entt::null || *active != m_drag->entity || !scene.registry().all_of<ColliderComponent>(entity))) {
        cancel(context);
    }
    if (m_drag) {
        updateDrag(context, lens, viewSize, input);
    }
    if (entity == entt::null || !scene.registry().all_of<ColliderComponent, TransformComponent>(entity) ||
        !SceneVisibility(scene, SceneVisibilityMode::Editor).drawn(entity)) {
        return m_drag.has_value();
    }
    scene.updateWorldTransforms();
    const ColliderComponent& collider = scene.registry().get<ColliderComponent>(entity);
    const glm::mat4& world = scene.registry().get<TransformComponent>(entity).world;
    const std::vector<ColliderHandle> handles = colliderHandles(collider, world);
    const glm::mat4 viewProjection =
        lensProjectionMatrix(lens, viewSize.x / std::max(viewSize.y, 1.0f)) * lensViewMatrix(lens);
    std::vector<std::optional<glm::vec2>> projected;
    float nearest = hitRadius;
    for (size_t index = 0; index < handles.size(); ++index) {
        projected.push_back(project(viewProjection, viewSize, handles[index].position));
        if (m_drag || !input.available || !projected.back()) {
            continue;
        }
        const float pixels = glm::distance(*projected.back(), input.mouse);
        if (pixels <= nearest) {
            nearest = pixels;
            m_hovered = index;
        }
    }
    if (!m_drag && m_hovered && input.leftPressed) {
        const ColliderHandle& handle = handles[*m_hovered];
        const std::optional<float> parameter =
            colliderHandleAxisParameter(lens, viewSize, input.mouse, handle.position, handle.direction);
        Drag drag{.entity = *active, .handle = handle, .start = collider, .startParameter = parameter.value_or(0.0f)};
        drag.scale = shapeFrame(collider, world).scale;
        if (parameter && context.editing.captureEntities(std::span(&entity, 1), drag.before)) {
            m_drag = std::move(drag);
        }
    }
    if (drawList != nullptr) {
        const float artPixel = editorArtPixel();
        const float half = 3.0f * artPixel;
        for (size_t index = 0; index < handles.size(); ++index) {
            if (!projected[index]) {
                continue;
            }
            const bool highlighted =
                m_hovered == index ||
                (m_drag && m_drag->handle.dimension == handles[index].dimension &&
                 m_drag->handle.axis == handles[index].axis && m_drag->handle.side == handles[index].side);
            const glm::vec2 center = glm::floor(viewPosition + *projected[index]);
            const ImVec2 min{center.x - half, center.y - half};
            const ImVec2 max{center.x + half, center.y + half};
            pixel::rect(drawList, min, max, toImU32(highlighted ? palette::overlayActive : palette::overlayCollider));
            pixel::outline(drawList, min, max, toImU32(palette::captionOutline), artPixel);
        }
    }
    return m_hovered.has_value() || m_drag.has_value();
}

void ColliderHandleTool::updateDrag(EditorContext& context, const RenderView& lens, glm::vec2 viewSize,
                                    const ColliderHandleInput& input) {
    if (input.cancel) {
        cancel(context);
        return;
    }
    Scene& scene = context.app.scene;
    const Entity entity = scene.findEntity(m_drag->entity);
    ColliderComponent& collider = scene.registry().get<ColliderComponent>(entity);
    if (const std::optional<float> parameter = colliderHandleAxisParameter(
            lens, viewSize, input.mouse, m_drag->handle.position, m_drag->handle.direction)) {
        const ColliderHandleDrag mode = input.ctrl ? ColliderHandleDrag::OneSided : ColliderHandleDrag::Symmetric;
        collider =
            dragColliderHandle(m_drag->start, m_drag->handle, m_drag->scale, *parameter - m_drag->startParameter, mode);
        m_drag->changed = true;
    }
    if (!input.leftDown) {
        if (m_drag->changed) {
            context.editing.commit("Edit Collider", {}, std::move(m_drag->before));
        }
        m_drag.reset();
    }
}

void ColliderHandleTool::cancel(EditorContext& context) {
    if (!m_drag) {
        return;
    }
    Scene& scene = context.app.scene;
    const Entity entity = scene.findEntity(m_drag->entity);
    if (entity != entt::null) {
        if (auto* collider = scene.registry().try_get<ColliderComponent>(entity)) {
            *collider = m_drag->start;
        }
    }
    m_drag.reset();
}

bool ColliderHandleTool::dragging() const {
    return m_drag.has_value();
}

std::optional<size_t> ColliderHandleTool::hovered() const {
    return m_hovered;
}
