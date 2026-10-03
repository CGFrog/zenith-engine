#include "SceneGizmoDraw.hpp"

#include "editor/interaction/EditorCamera.hpp"
#include "editor/theme/EditorPalette.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {
glm::vec4 toDebugColor(ThemeColor color) {
    return {color.r, color.g, color.b, color.a};
}

enum class SelectionRole : uint8_t {
    None,
    Selected,
    Active,
};

// Cone half-angles at or past 90 degrees have no finite base.
constexpr float maximumConeHalfAngle = glm::radians(89.0f);

glm::vec3 normalizedOr(const glm::vec3& direction, const glm::vec3& fallback) {
    const float length = glm::length(direction);
    return length > 1e-6f && std::isfinite(length) ? direction / length : fallback;
}

// A unit vector perpendicular to the unit vector `axis`.
glm::vec3 perpendicular(const glm::vec3& axis) {
    const glm::vec3 reference = std::abs(axis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(glm::cross(reference, axis));
}

// A world matrix split like PhysicsWorld splits a body's: translation,
// rotation, and the absolute per-axis scale. A mirrored matrix negates all
// three axes, as glm::decompose does, so the rotation stays proper and
// matches the body's. Shear is ignored.
struct RigidTransform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

RigidTransform splitTransform(const glm::mat4& world) {
    RigidTransform result;
    result.translation = glm::vec3(world[3]);
    glm::mat3 axes(world);
    for (int axis = 0; axis < 3; ++axis) {
        result.scale[axis] = glm::length(axes[axis]);
        axes[axis] = result.scale[axis] > 1e-12f ? axes[axis] / result.scale[axis] : glm::vec3(0.0f);
    }
    if (glm::determinant(axes) < 0.0f) {
        axes = -axes;
    }
    result.rotation = glm::normalize(glm::quat_cast(axes));
    return result;
}

// Records one viewport's overlays; holds the per-call lookups.
class GizmoRecorder {
  public:
    GizmoRecorder(const SceneGizmoView& view, DebugDraw& output)
        : m_view(view)
        , m_registry(view.scene.registry())
        , m_visibility(view.scene, SceneVisibilityMode::Editor)
        , m_colors(sceneGizmoColors())
        , m_output(output) {
        for (const SceneEntityId id : view.selection) {
            m_selected.insert(id.value);
        }
    }

    void lights(OverlayScope scope);
    void cameras(OverlayScope scope);
    void colliders(OverlayScope scope);
    void reflectionProbes(OverlayScope scope);
    void decals(OverlayScope scope);
    void irradianceVolumes(OverlayScope scope);
    void selectionBounds();
    void names(OverlayScope scope);

  private:
    SelectionRole role(Entity entity) const {
        const SceneEntityId id = m_view.scene.id(entity);
        if (!m_view.selection.empty() && m_view.selection.front() == id) {
            return SelectionRole::Active;
        }
        return m_selected.contains(id.value) ? SelectionRole::Selected : SelectionRole::None;
    }

    // Whether a category of `scope` draws the entity: visible in the
    // editor, and selected when the scope asks for that.
    bool shown(OverlayScope scope, Entity entity) const {
        if (scope == OverlayScope::Off || !m_visibility.drawn(entity)) {
            return false;
        }
        return scope == OverlayScope::All || role(entity) != SelectionRole::None;
    }

    // Selection draws over scene surfaces in the selection hues; everything
    // else is depth tested in its category colour.
    DebugStyle style(const glm::vec4& categoryColor, Entity entity) const {
        switch (role(entity)) {
        case SelectionRole::Active:
            return {.color = m_colors.active, .depth = DebugDepth::AlwaysOnTop};
        case SelectionRole::Selected:
            return {.color = m_colors.selected, .depth = DebugDepth::AlwaysOnTop};
        case SelectionRole::None:
            break;
        }
        return {.color = categoryColor, .depth = DebugDepth::Tested};
    }

    const glm::mat4& world(Entity entity) const {
        return m_registry.get<TransformComponent>(entity).world;
    }

    void marker(const glm::vec3& position, const DebugStyle& lineStyle) {
        m_output.sphere(position, sceneGizmoSizes::markerRadius, lineStyle, sceneGizmoSegments::marker);
    }

    void collider(const ColliderComponent& collider, const glm::mat4& entityWorld, const DebugStyle& lineStyle);
    void triangleMesh(const MeshAsset& mesh, const glm::mat4& shapeWorld, const DebugStyle& lineStyle);
    // Grows lower/upper by the world bounds of entity's mesh, if it has one
    // the editor draws.
    void growMeshBounds(Entity entity, glm::vec3& lower, glm::vec3& upper) const;

    const SceneGizmoView& m_view;
    const entt::registry& m_registry;
    SceneVisibility m_visibility;
    SceneGizmoColors m_colors;
    DebugDraw& m_output;
    std::unordered_set<uint64_t> m_selected;
};

void GizmoRecorder::lights(OverlayScope scope) {
    for (const auto [entity, light, transform] :
         m_registry.view<const PointLightComponent, const TransformComponent>().each()) {
        if (shown(scope, entity)) {
            const DebugStyle lineStyle = style(m_colors.light, entity);
            const glm::vec3 position(transform.world[3]);
            m_output.sphere(position, std::max(light.range, 0.0f), lineStyle, sceneGizmoSegments::range);
            marker(position, lineStyle);
        }
    }
    for (const auto [entity, light, transform] :
         m_registry.view<const SpotLightComponent, const TransformComponent>().each()) {
        if (shown(scope, entity)) {
            const DebugStyle lineStyle = style(m_colors.light, entity);
            const glm::vec3 position(transform.world[3]);
            // As the renderer: the local direction turned by the world matrix.
            const glm::vec3 direction =
                normalizedOr(glm::mat3(transform.world) * light.direction, glm::vec3(0.0f, -1.0f, 0.0f));
            const float range = std::max(light.range, 0.0f);
            for (const float halfAngle : {light.outerConeRadians, light.innerConeRadians}) {
                m_output.cone(position, direction, range, std::clamp(halfAngle, 0.0f, maximumConeHalfAngle), lineStyle,
                              sceneGizmoSegments::range);
            }
            marker(position, lineStyle);
        }
    }
    for (const auto [entity, light, transform] :
         m_registry.view<const DirectionalLightComponent, const TransformComponent>().each()) {
        if (shown(scope, entity)) {
            const DebugStyle lineStyle = style(m_colors.light, entity);
            const glm::vec3 position(transform.world[3]);
            // The component points toward the light in world space; its
            // transform only places the gizmo. Arrows show the light's travel.
            const glm::vec3 travel =
                -normalizedOr(light.direction, glm::normalize(DirectionalLightComponent{}.direction));
            const glm::vec3 side = perpendicular(travel) * sceneGizmoSizes::markerRadius;
            m_output.circle(position, travel, sceneGizmoSizes::markerRadius, lineStyle, sceneGizmoSegments::marker);
            for (const glm::vec3& offset : {glm::vec3(0.0f), side, -side}) {
                m_output.arrow(position + offset, position + offset + travel * sceneGizmoSizes::directionalArrowLength,
                               lineStyle);
            }
        }
    }
    for (const Entity entity : m_registry.view<const EnvironmentLightComponent, const TransformComponent>()) {
        if (shown(scope, entity)) {
            marker(glm::vec3(world(entity)[3]), style(m_colors.environment, entity));
        }
    }
}

void GizmoRecorder::cameras(OverlayScope scope) {
    // The camera the renderer views a scene through without an override: the
    // first primary one in the same iteration order (MeshPass).
    Entity activeCamera = entt::null;
    auto cameraView = m_registry.view<const CameraComponent, const TransformComponent>();
    for (const Entity entity : cameraView) {
        if (cameraView.get<const CameraComponent>(entity).primary) {
            activeCamera = entity;
            break;
        }
    }
    for (const Entity entity : cameraView) {
        if (!shown(scope, entity) || (m_view.viewCamera && m_view.scene.id(entity) == *m_view.viewCamera)) {
            continue;
        }
        std::optional<RenderView> lens = sceneCameraLens(m_view.scene, entity);
        if (!lens || !(lens->nearPlane > 0.0f) || !(lens->farPlane > lens->nearPlane)) {
            continue;
        }
        const glm::vec4& color = entity == activeCamera ? m_colors.activeCamera : m_colors.camera;
        const DebugStyle lineStyle = style(color, entity);
        const float displayDepth = std::min(lens->farPlane, lens->nearPlane + sceneGizmoSizes::cameraDisplayDepth);
        RenderView drawn = *lens;
        drawn.farPlane = role(entity) == SelectionRole::None ? displayDepth : lens->farPlane;
        m_output.frustum(lensProjectionMatrix(drawn, m_view.aspectRatio) * lensViewMatrix(drawn), lineStyle);
        // Blender's up triangle, always at the display depth so it keeps
        // its size when a selected camera's frustum reaches its far plane.
        const float halfHeight = displayDepth * std::tan(lens->verticalFovRadians * 0.5f);
        const float halfWidth = halfHeight * m_view.aspectRatio;
        const glm::vec3 up = normalizedOr(lens->up, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 right = normalizedOr(glm::cross(lens->forward, up), glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 base = lens->position + lens->forward * displayDepth + up * halfHeight * 1.1f;
        const glm::vec3 left = base - right * halfWidth * 0.5f;
        const glm::vec3 rightCorner = base + right * halfWidth * 0.5f;
        const glm::vec3 apex = base + up * halfHeight * 0.5f;
        m_output.line(left, rightCorner, lineStyle);
        m_output.line(rightCorner, apex, lineStyle);
        m_output.line(apex, left, lineStyle);
    }
}

void GizmoRecorder::colliders(OverlayScope scope) {
    for (const auto [entity, collider, transform] :
         m_registry.view<const ColliderComponent, const TransformComponent>().each()) {
        if (shown(scope, entity)) {
            this->collider(collider, transform.world, style(m_colors.collider, entity));
        }
    }
}

void GizmoRecorder::collider(const ColliderComponent& collider, const glm::mat4& entityWorld,
                             const DebugStyle& lineStyle) {
    const RigidTransform body = splitTransform(entityWorld);
    const glm::vec3& scale = body.scale;
    std::visit(
        [&](const auto& shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, BoxCollider>) {
                m_output.box(body.translation, shape.halfExtents * scale, body.rotation, lineStyle);
            } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                m_output.sphere(body.translation, shape.radius * std::max({scale.x, scale.y, scale.z}), lineStyle,
                                sceneGizmoSegments::range);
            } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                // Jolt capsules run along their local Y axis.
                const glm::vec3 spine = body.rotation * glm::vec3(0.0f, shape.halfHeight * scale.y, 0.0f);
                m_output.capsule(body.translation - spine, body.translation + spine,
                                 shape.radius * std::max(scale.x, scale.z), lineStyle, sceneGizmoSegments::range);
            } else {
                if (const MeshAsset* mesh = m_view.assets.mesh(shape.mesh)) {
                    const glm::mat4 shapeWorld = glm::translate(glm::mat4(1.0f), body.translation) *
                                                 glm::mat4_cast(body.rotation) * glm::scale(glm::mat4(1.0f), scale);
                    triangleMesh(*mesh, shapeWorld, lineStyle);
                }
            }
        },
        collider.shape);
}

void GizmoRecorder::triangleMesh(const MeshAsset& mesh, const glm::mat4& shapeWorld, const DebugStyle& lineStyle) {
    // The triangles PhysicsWorld collides with: the full-detail index ranges
    // (simplified LODs follow them in the same buffer), or the raw vertex
    // list when unindexed.
    std::vector<std::pair<size_t, size_t>> ranges;
    if (mesh.indices.empty()) {
        ranges.emplace_back(0, mesh.vertices.size());
    } else if (mesh.primitives.empty()) {
        ranges.emplace_back(0, mesh.indices.size());
    } else {
        for (const MeshPrimitive& primitive : mesh.primitives) {
            const size_t begin = std::min<size_t>(primitive.firstIndex, mesh.indices.size());
            ranges.emplace_back(begin, std::min(begin + primitive.indexCount, mesh.indices.size()));
        }
    }
    size_t triangles = 0;
    for (const auto& [begin, end] : ranges) {
        triangles += (end - begin) / 3;
    }
    glm::vec3 lower(std::numeric_limits<float>::max());
    glm::vec3 upper(std::numeric_limits<float>::lowest());
    for (const Vertex& vertex : mesh.vertices) {
        lower = glm::min(lower, vertex.position);
        upper = glm::max(upper, vertex.position);
    }
    if (mesh.vertices.empty()) {
        return;
    }
    if (triangles > maximumColliderWireTriangles) {
        m_output.box(shapeWorld * glm::translate(glm::mat4(1.0f), (lower + upper) * 0.5f), (upper - lower) * 0.5f,
                     lineStyle);
        return;
    }
    const auto position = [&](size_t index) {
        const uint32_t vertex = mesh.indices.empty() ? static_cast<uint32_t>(index) : mesh.indices[index];
        return vertex < mesh.vertices.size()
                   ? std::optional<glm::vec3>(glm::vec3(shapeWorld * glm::vec4(mesh.vertices[vertex].position, 1.0f)))
                   : std::nullopt;
    };
    for (const auto& [begin, end] : ranges) {
        for (size_t index = begin; index + 2 < end; index += 3) {
            const std::optional<glm::vec3> a = position(index);
            const std::optional<glm::vec3> b = position(index + 1);
            const std::optional<glm::vec3> c = position(index + 2);
            if (a && b && c) {
                m_output.line(*a, *b, lineStyle);
                m_output.line(*b, *c, lineStyle);
                m_output.line(*c, *a, lineStyle);
            }
        }
    }
}

void GizmoRecorder::reflectionProbes(OverlayScope scope) {
    // Axis-aligned at the entity's position; rotation and scale are ignored.
    for (const auto [entity, probe, transform] :
         m_registry.view<const ReflectionProbeComponent, const TransformComponent>().each()) {
        if (shown(scope, entity)) {
            const glm::vec3 center(transform.world[3]);
            m_output.aabb(center - probe.halfExtents, center + probe.halfExtents,
                          style(m_colors.reflectionProbe, entity));
        }
    }
}

void GizmoRecorder::decals(OverlayScope scope) {
    // The world transform applied to [-halfExtents, halfExtents], projecting
    // along the box's local -Y axis.
    for (const auto [entity, decal, transform] :
         m_registry.view<const DecalComponent, const TransformComponent>().each()) {
        if (shown(scope, entity)) {
            const DebugStyle lineStyle = style(m_colors.decal, entity);
            m_output.box(transform.world, decal.halfExtents, lineStyle);
            const glm::vec3 top(transform.world * glm::vec4(0.0f, decal.halfExtents.y, 0.0f, 1.0f));
            const glm::vec3 bottom(transform.world * glm::vec4(0.0f, -decal.halfExtents.y, 0.0f, 1.0f));
            m_output.arrow(top, bottom, lineStyle);
        }
    }
}

void GizmoRecorder::irradianceVolumes(OverlayScope scope) {
    // Axis-aligned at the entity's position; rotation and scale are ignored.
    for (const auto [entity, volume, transform] :
         m_registry.view<const IrradianceVolumeComponent, const TransformComponent>().each()) {
        if (shown(scope, entity)) {
            const glm::vec3 center(transform.world[3]);
            m_output.aabb(center - volume.halfExtents, center + volume.halfExtents,
                          style(m_colors.irradianceVolume, entity));
        }
    }
}

void GizmoRecorder::growMeshBounds(Entity entity, glm::vec3& lower, glm::vec3& upper) const {
    const auto* renderer = m_registry.try_get<MeshRendererComponent>(entity);
    const MeshAsset* mesh = renderer != nullptr ? m_view.assets.mesh(renderer->mesh) : nullptr;
    if (mesh == nullptr || mesh->vertices.empty() || !m_visibility.drawn(entity)) {
        return;
    }
    glm::vec3 localLower(std::numeric_limits<float>::max());
    glm::vec3 localUpper(std::numeric_limits<float>::lowest());
    for (const Vertex& vertex : mesh->vertices) {
        localLower = glm::min(localLower, vertex.position);
        localUpper = glm::max(localUpper, vertex.position);
    }
    const glm::mat4& entityWorld = world(entity);
    for (uint32_t corner = 0; corner < 8u; ++corner) {
        const glm::vec3 local{(corner & 1u) != 0u ? localUpper.x : localLower.x,
                              (corner & 2u) != 0u ? localUpper.y : localLower.y,
                              (corner & 4u) != 0u ? localUpper.z : localLower.z};
        const glm::vec3 point(entityWorld * glm::vec4(local, 1.0f));
        lower = glm::min(lower, point);
        upper = glm::max(upper, point);
    }
}

void GizmoRecorder::selectionBounds() {
    // One world-space box per selected object around its own mesh and its
    // descendants' (a model's meshes sit on child entities).
    std::vector<Entity> pending;
    for (const SceneEntityId id : m_view.selection) {
        const Entity selected = m_view.scene.findEntity(id);
        if (selected == entt::null || !m_visibility.drawn(selected)) {
            continue;
        }
        glm::vec3 lower(std::numeric_limits<float>::max());
        glm::vec3 upper(std::numeric_limits<float>::lowest());
        pending.assign(1, selected);
        while (!pending.empty()) {
            const Entity entity = pending.back();
            pending.pop_back();
            growMeshBounds(entity, lower, upper);
            if (const auto* relationship = m_registry.try_get<RelationshipComponent>(entity)) {
                pending.insert(pending.end(), relationship->children.begin(), relationship->children.end());
            }
        }
        if (lower.x <= upper.x) {
            m_output.aabb(lower, upper, style(m_colors.selected, selected));
        }
    }
}

void GizmoRecorder::names(OverlayScope scope) {
    for (const auto [entity, name, transform] :
         m_registry.view<const NameComponent, const TransformComponent>().each()) {
        // A model's node entities would bury the scene in labels.
        if (!shown(scope, entity) || (scope == OverlayScope::All && m_registry.all_of<ModelNodeComponent>(entity))) {
            continue;
        }
        const SelectionRole entityRole = role(entity);
        const glm::vec4& color = entityRole == SelectionRole::Active     ? m_colors.active
                                 : entityRole == SelectionRole::Selected ? m_colors.selected
                                                                         : m_colors.label;
        m_output.label(glm::vec3(transform.world[3]), name.name, color);
    }
}
} // namespace

SceneGizmoColors sceneGizmoColors() {
    return {.light = toDebugColor(palette::overlayLight),
            .environment = toDebugColor(palette::overlayEnvironment),
            .camera = toDebugColor(palette::overlayCamera),
            .activeCamera = toDebugColor(palette::overlayActiveCamera),
            .collider = toDebugColor(palette::overlayCollider),
            .reflectionProbe = toDebugColor(palette::overlayReflectionProbe),
            .decal = toDebugColor(palette::overlayDecal),
            .irradianceVolume = toDebugColor(palette::overlayIrradianceVolume),
            .selected = toDebugColor(palette::overlaySelected),
            .active = toDebugColor(palette::overlayActive),
            .label = toDebugColor(palette::overlayLabel)};
}

void drawSceneGizmos(const SceneGizmoView& view, const SceneGizmoOverlays& overlays, DebugDraw& output) {
    GizmoRecorder recorder(view, output);
    recorder.lights(overlays.lights);
    recorder.cameras(overlays.cameras);
    recorder.colliders(overlays.colliders);
    recorder.reflectionProbes(overlays.reflectionProbes);
    recorder.decals(overlays.decals);
    recorder.irradianceVolumes(overlays.irradianceVolumes);
    if (overlays.selectionBounds) {
        recorder.selectionBounds();
    }
    recorder.names(overlays.names);
}
