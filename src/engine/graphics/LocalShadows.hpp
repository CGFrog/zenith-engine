#pragma once

// Shadows of point and spot lights share one square depth atlas packed by allocateLocalShadowAtlas.

#include "engine/graphics/Lighting.hpp"
#include "engine/graphics/Rendering.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Shadows of point and spot lights share one square depth atlas packed by
// allocateLocalShadowAtlas. This file holds the renderer-independent policy:
// which lights get tiles, the tile projections, and which cached tiles are
// re-rendered each frame. LocalShadowPass owns the GPU side.
//
// Conventions (mirrored by shaders/local_shadow.vert and shaders/mesh.frag):
// - A tile's view-projection maps world space to Vulkan clip space with
//   conventional depth (0 at the near plane, 1 at the light's range) and Y
//   flipped like the directional cascades, so NDC xy in [-1, 1] covers the
//   whole tile and uv = ndc * 0.5 + 0.5 has v = 0 at the tile's top row.
// - A spot light has one perspective tile looking along its direction.
// - A point light has six tiles in the face order +X, -X, +Y, -Y, +Z, -Z. A
//   world point is shadowed by the face of its major axis from the light
//   (localShadowFace); each face's field of view is slightly wider than 90
//   degrees so filtering near a face edge stays inside the tile.

// Quality-dependent atlas limits. Resolutions are tile edges in texels.
struct LocalShadowProfile {
    // Square atlas edge; zero disables local shadows.
    uint32_t atlasSize{0};
    uint32_t spotResolution{0};
    // Per cube face.
    uint32_t pointResolution{0};
    // Upper bound for per-light resolution hints.
    uint32_t maximumResolution{0};
    // Shadowed lights per frame; the most important casters are kept.
    uint32_t maximumLights{0};
    // Tiles re-rendered per frame; the remaining dirty tiles keep their
    // previous contents until a later frame.
    uint32_t updateBudgetTiles{0};
};

LocalShadowProfile localShadowProfile(LocalShadowQuality quality);

inline constexpr uint32_t pointShadowFaceCount = 6;

uint32_t localShadowFaceCount(LocalLightType type);

// Atlas requests for the shadow-casting lights, at most profile.maximumLights.
// Selection keeps the highest shadowPriority, then the lights whose range
// reaches closest to the camera. The returned order, which the allocator
// preserves through descending priorities, packs spot lights before point
// lights and is otherwise independent of the camera, so atlas placements stay
// put (and cached tiles stay valid) while the selected set is unchanged.
std::vector<LocalShadowRequest> buildLocalShadowRequests(std::span<const LocalLight> lights,
                                                         const glm::vec3& cameraPosition,
                                                         const LocalShadowProfile& profile);

// Point-light face (0..5, see the face order above) that shadows a point at
// lightToPoint from the light.
uint32_t localShadowFace(const glm::vec3& lightToPoint);

// Tangent of half the tile's field of view; one shadow texel then spans
// 2 * tangent * distance / tileSize world units at a distance from the light.
float localShadowTanHalfFov(const LocalLight& light);

// World-to-clip transform of one tile; face must be 0 for spot lights.
glm::mat4 localShadowViewProjection(const LocalLight& light, uint32_t face);

// A world-space bounding sphere around shadow-casting geometry that changed
// since the previous frame (its old and new positions both need reporting).
struct ShadowCasterBounds {
    glm::vec3 center{0.0f};
    float radius{0.0f};
};

// One atlas tile and the state of its cached contents.
struct CachedLocalShadowTile {
    LocalShadowTile tile;
    LocalLightType type{LocalLightType::Spot};
    glm::vec3 lightPosition{0.0f};
    float lightRange{0.0f};
    float tanHalfFov{1.0f};
    // This frame's projection of the light.
    glm::mat4 viewProjection{1.0f};
    // Projection the atlas contents were rendered with; meaningful only when
    // rendered. Shading always uses this one, so a light that moved while its
    // tile waited for the update budget casts its previous, consistent shadow.
    glm::mat4 renderedViewProjection{1.0f};
    // The atlas holds this tile's depth at its current placement.
    bool rendered{false};
    // Rendered contents that no longer match the scene or the light.
    bool stale{false};
};

// Tracks which atlas tiles hold valid depth across frames so static casters
// are drawn once. Per frame: synchronize -> scheduleUpdates -> render the
// scheduled lights -> markRendered for each light actually drawn.
//
// Invariants: tiles() follows the atlas tile order, so a light's faces are
// contiguous with face 0 first; a light is shadowed (shadowTile) only when
// every one of its faces is rendered at its current placement.
class LocalShadowCache {
  public:
    // Adopts this frame's atlas. Tiles that kept their placement and light type
    // keep their contents; they become stale when the light's projection
    // changed, when a changed caster touches the light's range, or when
    // invalidateAll is set (casters were rebuilt). Lights missing from
    // `lights` are ignored.
    void synchronize(const LocalShadowAtlas& atlas, std::span<const LocalLight> lights,
                     std::span<const ShadowCasterBounds> changedCasters, bool invalidateAll);
    // Lights whose tiles should be drawn this frame, most urgent first: lights
    // without valid contents, then stale ones, each nearest the camera first.
    // Whole lights are scheduled while their tiles fit in budgetTiles; the
    // first light is always scheduled so a budget below six tiles cannot
    // starve point lights.
    std::vector<uint64_t> scheduleUpdates(const glm::vec3& cameraPosition, uint32_t budgetTiles) const;
    // Records that every tile of the light now holds depth rendered with its
    // current viewProjection.
    void markRendered(uint64_t stableId);
    // Discards the light's contents (for example after a partial redraw), so
    // it is unshadowed until rendered again.
    void invalidate(uint64_t stableId);

    std::span<const CachedLocalShadowTile> tiles() const {
        return m_tiles;
    }

    // Index into tiles() of the light's face 0, regardless of its state.
    std::optional<uint32_t> firstTile(uint64_t stableId) const;
    // firstTile when every face of the light is rendered, otherwise empty.
    std::optional<uint32_t> shadowTile(uint64_t stableId) const;
    // Tiles that are unrendered or stale.
    uint32_t pendingTileCount() const;
    void clear();

  private:
    std::vector<CachedLocalShadowTile> m_tiles;
    std::unordered_map<uint64_t, uint32_t> m_firstTiles;
};
