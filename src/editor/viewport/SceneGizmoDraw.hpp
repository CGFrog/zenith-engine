#pragma once

#include "engine/graphics/DebugDraw.hpp"
#include "engine/graphics/RenderIds.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

class AssetManager;

// Which objects one overlay category draws.
enum class OverlayScope : uint8_t {
    Off,
    // Only selected objects.
    Selected,
    All,
};

// The scene overlay categories of one viewport (its Overlays menu). Cheap
// helpers default to every object, and only the selection draws names.
struct SceneGizmoOverlays {
    // Point range spheres, spot cones, directional arrows, environment markers.
    OverlayScope lights{OverlayScope::All};
    // A small fixed-size icon, with dotted rays along the view when selected;
    // the active scene camera is highlighted.
    OverlayScope cameras{OverlayScope::All};
    // Unselected colliders draw faintly (a triangle mesh only as its bounds),
    // so invisible collision objects stay findable.
    OverlayScope colliders{OverlayScope::All};
    OverlayScope reflectionProbes{OverlayScope::All};
    OverlayScope decals{OverlayScope::All};
    OverlayScope irradianceVolumes{OverlayScope::All};
    // Object names anchored at their origins.
    OverlayScope names{OverlayScope::Selected};
    // World-space boxes around the meshes of selected objects.
    bool selectionBounds{true};

    bool operator==(const SceneGizmoOverlays&) const = default;
};

// Tessellation of the overlay shapes, fixed so recordings are deterministic.
namespace sceneGizmoSegments {
// Range spheres, cones, and collider spheres and capsules.
inline constexpr uint32_t range = 32;
// The small marker spheres and circles at light and environment origins.
inline constexpr uint32_t marker = 16;
} // namespace sceneGizmoSegments

// Overlay shape sizes in meters.
namespace sceneGizmoSizes {
inline constexpr float markerRadius = 0.2f;
inline constexpr float directionalArrowLength = 1.5f;
// The camera icon: a pyramid of this depth and base half height at a fixed
// aspect, whatever the camera's lens or the viewport's size.
inline constexpr float cameraIconDepth = 0.5f;
inline constexpr float cameraIconHalfHeight = 0.25f;
inline constexpr float cameraIconAspect = 1.5f;
// How far a selected camera's dotted rays reach (at most its far plane), and
// the dash and gap lengths of dotted overlay lines.
inline constexpr float cameraRayLength = 6.0f;
inline constexpr float dashLength = 0.1f;
inline constexpr float dashGap = 0.2f;
} // namespace sceneGizmoSizes

// Triangle mesh colliders with more triangles than this draw their oriented
// bounds instead of every edge.
inline constexpr uint32_t maximumColliderWireTriangles = 4096;

// What one viewport shows and how. The scene's world transforms must be
// current; the scene and assets must outlive the call.
struct SceneGizmoView {
    const Scene& scene;
    const AssetManager& assets;
    // The editor selection, active entity first.
    std::span<const SceneEntityId> selection;
    // The scene camera the view looks through, whose own frustum is skipped.
    std::optional<SceneEntityId> viewCamera;
    // The sorted render objects the view's local view isolates (as
    // RenderRuntime::setViewVisibleEntities takes them); null when the view
    // is not isolated. Entities outside it draw no gizmos, labels, or bounds.
    const std::vector<RenderObjectId>* isolatedObjects{nullptr};
};

// Records the enabled overlay categories of view's scene into output (a
// viewport's RenderRuntime::viewDebugDraw list), plus a label per name. Only
// entities drawn in the editor (SceneVisibility, Editor mode) and inside the
// view's local-view isolation contribute, so hidden objects show no overlays.
// Selection bounds cover a selected object's meshes, its descendants', and
// the members its collection instances draw, from each mesh's cached box. Colours are the palette's overlay tokens:
// a category colour, overlayActive for the active entity, overlaySelected for
// the rest of the selection, which also draws over scene surfaces. Objects
// with no surface of their own, colliders and directional lights, draw
// faintly (palette::overlayIdleAlpha) when not selected.
//
// Colliders are drawn from ColliderComponent, not PhysicsWorld::drawDebug:
// the editor steps physics only in Play, so in Edit the physics world holds
// no (or stale) bodies, and drawDebug cannot filter by selection or hide.
// Shapes follow PhysicsWorld's body construction: at the entity, rotated by
// its world rotation, moved and turned by the collider's offset
// (colliderLocalTransform), sized by its world scale.
void drawSceneGizmos(const SceneGizmoView& view, const SceneGizmoOverlays& overlays, DebugDraw& output);

// The overlay colours as DebugDraw colours (the palette tokens).
struct SceneGizmoColors {
    glm::vec4 light;
    glm::vec4 environment;
    glm::vec4 camera;
    glm::vec4 activeCamera;
    glm::vec4 collider;
    glm::vec4 reflectionProbe;
    glm::vec4 decal;
    glm::vec4 irradianceVolume;
    glm::vec4 selected;
    glm::vec4 active;
    glm::vec4 label;
};

SceneGizmoColors sceneGizmoColors();

// Where the overlays mark an object: the origin of each light (point, spot,
// directional, environment), camera, reflection probe, decal, and
// irradiance volume whose category view would draw (the same scopes,
// visibility, isolation, and skipped view camera as drawSceneGizmos), in
// scene order. Viewport picking tests clicks against them, so objects
// without a mesh can be selected.
struct SceneGizmoAnchor {
    SceneEntityId entity;
    glm::vec3 position{0.0f};
};

std::vector<SceneGizmoAnchor> sceneGizmoAnchors(const SceneGizmoView& view, const SceneGizmoOverlays& overlays);
