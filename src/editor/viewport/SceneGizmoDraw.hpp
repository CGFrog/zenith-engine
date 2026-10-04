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
// helpers default to every object; collider wireframes, which can be dense,
// default to the selection.
struct SceneGizmoOverlays {
    // Point range spheres, spot cones, directional arrows, environment markers.
    OverlayScope lights{OverlayScope::All};
    // Frustums at the view's aspect; the active scene camera is highlighted.
    OverlayScope cameras{OverlayScope::All};
    OverlayScope colliders{OverlayScope::Selected};
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
// How deep an unselected camera's frustum is drawn; a selected camera shows
// its whole near-to-far volume.
inline constexpr float cameraDisplayDepth = 1.5f;
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
    // Width over height of the view, for camera frustums.
    float aspectRatio{1.0f};
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
// the rest of the selection, which also draws over scene surfaces.
//
// Colliders are drawn from ColliderComponent, not PhysicsWorld::drawDebug:
// the editor steps physics only in Play, so in Edit the physics world holds
// no (or stale) bodies, and drawDebug cannot filter by selection or hide.
// Shapes follow PhysicsWorld's body construction: centred on the entity,
// rotated by its world rotation, sized by its world scale.
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
