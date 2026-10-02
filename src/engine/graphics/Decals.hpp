#pragma once

#include "engine/graphics/Lighting.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Clustered decals: oriented box projectors (DecalComponent) assigned to the
// light-cluster grid by decal_clusters.comp and applied in mesh.frag before
// lighting, where they blend base color, normal, roughness and metallic. The
// functions below are the CPU reference of that shading and assignment.
//
// Decal space maps the projector box onto the cube [-1, 1]^3. The decal
// projects along -Y; texture u = x * 0.5 + 0.5 and v = z * 0.5 + 0.5, so the
// top of the image lies toward local -Z.

// Decals uploaded per frame; mirrors MAXIMUM_DECALS in mesh.frag and
// decal_clusters.comp. Scenes with more keep the nearest (see selectDecals).
inline constexpr uint32_t maximumDecals = 1024;
// Decals one cluster lists; mirrors MAXIMUM_DECALS_PER_CLUSTER. Each cluster
// owns a block of one count followed by this many indices.
inline constexpr uint32_t maximumDecalsPerCluster = 31;
// Fraction of the box's half depth (along the projection axis) over which a
// decal fades out toward the box's top and bottom faces; mirrors
// DECAL_DEPTH_FADE_FRACTION in mesh.frag.
inline constexpr float decalDepthFadeFraction = 0.25f;

struct DecalProjector {
    Entity entity{entt::null};
    // World space to decal space: the inverse of the entity's world transform
    // composed with the half-extent scale.
    glm::mat4 worldToDecal{1.0f};
    // World-space center and the radius of the box's bounding sphere.
    glm::vec3 center{0.0f};
    float boundingRadius{0.0f};
    int32_t sortOrder{0};
    TextureHandle albedoTexture{};
    TextureHandle normalTexture{};
    glm::vec3 tint{1.0f};
    float opacity{1.0f};
    float baseColorStrength{1.0f};
    float normalStrength{1.0f};
    float roughness{0.5f};
    float roughnessStrength{0.0f};
    float metallic{0.0f};
    float metallicStrength{0.0f};
    // Angle fade = clamp(cos * scale + bias, 0, 1), where cos is the cosine
    // between the surface normal and decalFacingAxis.
    float angleFadeScale{1.0f};
    float angleFadeBias{0.0f};
};

// The projector of a decal on an entity with the given world transform, or
// nothing when the box is degenerate (a zero or non-finite axis).
std::optional<DecalProjector> makeDecalProjector(const DecalComponent& decal, const glm::mat4& world);

// Every decal in the scene with a valid box, in application order: ascending
// sort order, then ascending stable scene id (SceneEntityId).
std::vector<DecalProjector> gatherDecals(const Scene& scene);

// Keeps at most maximumCount decals, preferring those whose bounding sphere
// is nearest the camera; the result stays in application order.
std::vector<DecalProjector> selectDecals(std::span<const DecalProjector> decals, const glm::vec3& cameraPosition,
                                         uint32_t maximumCount);

// Position of a world point in decal space ([-1, 1]^3 inside the box).
glm::vec3 decalSpacePosition(const DecalProjector& decal, const glm::vec3& worldPosition);
// Texture coordinate at a world point, or nothing outside the box.
std::optional<glm::vec2> decalUv(const DecalProjector& decal, const glm::vec3& worldPosition);
// World unit vector the decal projects against: surfaces whose normal points
// along it face the projector. Normal to the box's local XZ plane.
glm::vec3 decalFacingAxis(const DecalProjector& decal);
// 1 for surfaces facing the projector within the fade start angle, 0 beyond the
// end angle (including every back-facing surface), linear in the cosine between.
float decalAngleFade(const DecalProjector& decal, const glm::vec3& worldNormal);
// 1 in the middle of the box's depth, fading to 0 at its top and bottom faces.
float decalDepthFade(float decalSpaceY);
// Mask of the decal at a surface point before texture alpha: opacity times the
// angle and depth fades, zero outside the box.
float decalCoverage(const DecalProjector& decal, const glm::vec3& worldPosition, const glm::vec3& worldNormal);

// Conservative view-space box around the projector box for a rigid view.
ClusterBounds decalViewBounds(const DecalProjector& decal, const glm::mat4& view);
// Inclusive overlap test; mirrors decal_clusters.comp.
bool decalAffectsCluster(const ClusterBounds& decalViewBounds, const ClusterBounds& cluster);

struct ClusterDecalLists {
    // Per cluster: the number of decals listed, at most maximumDecalsPerCluster.
    std::vector<uint32_t> counts;
    // Per cluster: maximumDecalsPerCluster entries, the first counts[c] of
    // which are decal indices in ascending order.
    std::vector<uint32_t> indices;
    uint32_t maximumDecalsInCluster{0};
    // Assignments dropped because a cluster exceeded maximumDecalsPerCluster;
    // the decals applied last are the ones dropped.
    uint32_t discardedAssignments{0};
};

// CPU reference of decal_clusters.comp.
ClusterDecalLists assignDecalsToClusters(const ClusterGrid& grid, std::span<const ClusterBounds> decalViewBounds);
