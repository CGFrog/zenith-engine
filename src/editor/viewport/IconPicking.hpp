#pragma once

#include "editor/viewport/RegionSelect.hpp"
#include "editor/viewport/SceneGizmoDraw.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/graphics/Rendering.hpp"

#include <glm/glm.hpp>

#include <span>
#include <vector>

struct ViewportPickOutcome;
struct ViewportPickQuery;

// Clicking objects that have no mesh (lights, cameras, probes, decals,
// emitters) by the overlay marks at their origins. The id buffer and the
// mesh ray casts never see them, so a viewport tests the click (or box and
// lasso) against these points in screen space first.

// How far from an icon's projected origin a click still picks it, in view pixels.
inline constexpr float iconPickRadiusPixels = 10.0f;

// The icons a view can pick: the overlay anchors (sceneGizmoAnchors) plus
// the origins of audio emitters and listeners, particle emitters, and
// volumetric fog, limited to entities the editor may select
// (SceneVisibility::selectable) inside the view's isolation.
std::vector<SceneGizmoAnchor> pickableIcons(const SceneGizmoView& view, const SceneGizmoOverlays& overlays);

// The icons within radiusPixels of point (view pixels), nearest on screen
// first and nearer the lens on a tie; none behind the lens.
std::vector<SceneEntityId> iconsNearPoint(std::span<const SceneGizmoAnchor> icons, const RenderView& lens,
                                          glm::vec2 viewSize, glm::vec2 point, float radiusPixels);
// The icons whose origins project inside region, in icons order.
std::vector<SceneEntityId> iconsInRegion(std::span<const SceneGizmoAnchor> icons, const RenderView& lens,
                                         glm::vec2 viewSize, const RegionSelection& region);

// Puts the query's icon hits into a resolved pick. An icon under a click
// counts when no pickable surface along the click's ray is nearer than it
// (a light inside a wall does not steal the wall's clicks): a click then
// picks the nearest such icon instead of the mesh, and a cycle lists those
// icons first and the hidden ones after the meshes. A region adds the icons
// inside. No entity is listed twice.
void mergeIconPicks(const ViewportPickQuery& query, const Scene& scene, const AssetManager& assets,
                    ViewportPickOutcome& outcome);
