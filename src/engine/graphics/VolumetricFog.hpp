#pragma once

#include "Lighting.hpp"
#include "Rendering.hpp"

#include <glm/glm.hpp>

#include <span>
#include <vector>

// Froxel volumetric fog math shared by VolumetricFogPass, its shaders
// (volumetric_fog_inject.comp, volumetric_fog_integrate.comp, and the fog
// lookup in mesh.frag and skybox.frag), and the CPU tests.
//
// Froxel layout: the froxel grid subdivides the clustered-lighting grid
// (ClusterGrid in Lighting.hpp). Each cluster tile splits into subdivision.x
// by subdivision.y froxel columns and rows, and each cluster depth slice into
// subdivision.z froxel slices that follow the same exponential depth law
// between ClusterGrid::nearDepth and ClusterGrid::sliceEndDepth. Froxel
// (x, y, z) therefore lies inside cluster (x / sub.x, y / sub.y, z / sub.z),
// so the inject shader reads that cluster's light list directly.
//
// Coordinates: x grows right and y down across the internal render extent
// (matching gl_FragCoord and the cluster tiles); z grows with view depth, the
// positive distance along the camera's forward axis. The volume ends at
// sliceEndDepth; anything farther away (including the sky) receives the fog
// integrated up to that depth.
//
// Units: extinction and scattering coefficients are per world unit (meters);
// in-scattered radiance uses the scene's linear radiance units.
namespace volumetric_fog {

// Froxels per cluster along x, y, and depth; zero for Off.
glm::uvec3 froxelSubdivision(VolumetricFogQuality quality);
// Whether a quality jitters its samples and blends them with the reprojected
// previous frame. Without it every froxel is sampled at its center.
bool temporalReprojection(VolumetricFogQuality quality);
// Weight of the current frame when temporal reprojection blends it with valid
// history; the remainder comes from the reprojected previous frame.
inline constexpr float currentFrameWeight = 0.1f;

struct FroxelGrid {
    ClusterGrid clusters;
    glm::uvec3 subdivision{1u};

    glm::uvec3 dimensions() const;
    // Continuous froxel slice coordinate of a view depth: froxel slice z spans
    // [z, z + 1). Clamped to [0, dimensions().z].
    float sliceCoordinate(float viewDepth) const;
    // View depth at a continuous slice coordinate in [0, dimensions().z];
    // the inverse of sliceCoordinate inside the volume.
    float sliceDepth(float slice) const;
    // Shader slice lookup: slice = log(depth) * scale + bias, before clamping.
    glm::vec2 sliceScaleBias() const;
    // The cluster (column, row, slice) containing a froxel.
    glm::uvec3 clusterOf(const glm::uvec3& froxel) const;
    // Normalized w at which the integrated volume holds the medium between the
    // eye and viewDepth. Integrated texel z holds the medium up to the end of
    // froxel slice z, whose texel center is at (z + 0.5) / depth slices.
    float integratedW(float viewDepth) const;
};

FroxelGrid makeFroxelGrid(const ClusterGrid& clusters, const glm::uvec3& subdivision);

// Henyey-Greenstein phase function in 1/steradian. cosine is between the
// light's propagation direction and the direction scattered toward the eye;
// anisotropy g lies in (-1, 1), positive values scatter forward.
float henyeyGreenstein(float cosine, float anisotropy);

// Extinction coefficient of the authored height fog at a world height:
// fog.density at and below fog.baseHeight, decaying exponentially above it.
float heightFogExtinction(const VolumetricFogComponent& fog, float worldHeight);

// One froxel's medium: radiance scattered toward the eye per unit length
// (scattering coefficient times phase-weighted incident radiance) and the
// extinction coefficient.
struct FroxelMedium {
    glm::vec3 inScattering{0.0f};
    float extinction{0.0f};
};

// Light scattered toward the eye between the eye and the far end of a froxel,
// and the transmittance over the same path. Composite a surface or the sky
// behind it as color * transmittance + scattering.
struct IntegratedFroxel {
    glm::vec3 scattering{0.0f};
    float transmittance{1.0f};
};

// Front-to-back integration of one froxel column, mirroring
// volumetric_fog_integrate.comp. segmentLengths[z] is the length of the view
// ray inside froxel slice z. Each slice is integrated analytically assuming a
// homogeneous medium inside it, so the result is exact for homogeneous fog.
std::vector<IntegratedFroxel> integrateFroxelColumn(std::span<const FroxelMedium> media,
                                                    std::span<const float> segmentLengths);

} // namespace volumetric_fog
