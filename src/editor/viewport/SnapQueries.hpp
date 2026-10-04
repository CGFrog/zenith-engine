#pragma once

#include "editor/interaction/ModalTransform.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <vector>

// Snap targets under the mouse for modal transforms, the gizmo, and the 3D
// cursor, found on the CPU against the meshes the editor may pick
// (forEachPickableMesh: visible, selectable, inside a local view).
//
// Why the CPU: these queries run every frame of a drag and must skip the
// objects being moved. The GPU id buffer holds only the front-most surface
// per pixel, which during a snapping drag is usually the moved object itself,
// and arrives frames late; physics bodies only exist while simulating. Mesh
// ray casts answer at once and can look past excluded objects.

// The lens and view a query looks through. Points are view pixels from the
// image's top-left corner (continuous: pixel (x, y) spans [x, x + 1)).
struct SnapView {
    RenderView lens;
    glm::vec2 viewSize{1.0f};
    // Sorted render objects of a local view (as ViewportPanel isolates them);
    // null for everything.
    const std::vector<RenderObjectId>* isolatedObjects{nullptr};
};

// Entities that are never snap targets: the given ones and their
// descendants (what a transform moves).
std::vector<Entity> snapExclusions(const Scene& scene, std::span<const SceneEntityId> entities);

// The nearest surface point under viewPoint, skipping excluded entities
// (sorted, from snapExclusions), or nullopt when the ray hits nothing else.
std::optional<glm::vec3> surfaceSnapPoint(const Scene& scene, const AssetManager& assets, const SnapView& view,
                                          glm::vec2 viewPoint, std::span<const Entity> excluded = {});

// The mesh vertex (world position) that projects nearest to viewPoint within
// radiusPixels, preferring the nearer to the camera on a tie, skipping
// excluded entities; nullopt when none is that close.
std::optional<glm::vec3> vertexSnapPoint(const Scene& scene, const AssetManager& assets, const SnapView& view,
                                         glm::vec2 viewPoint, float radiusPixels,
                                         std::span<const Entity> excluded = {});

// Screen distance within which vertex snapping finds a vertex, in pixels.
inline constexpr float vertexSnapRadiusPixels = 24.0f;

// The target point for target under viewPoint: Surface and Vertex as above;
// nullopt for Increment, which has no point.
std::optional<glm::vec3> snapTargetPoint(SnapTarget target, const Scene& scene, const AssetManager& assets,
                                         const SnapView& view, glm::vec2 viewPoint, std::span<const Entity> excluded);
