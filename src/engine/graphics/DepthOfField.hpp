#pragma once

// Thin-lens circle-of-confusion math for depth of field.

#include "Rendering.hpp"

#include <glm/glm.hpp>

#include <cstdint>

// Thin-lens depth of field math shared by DepthOfFieldPass, its shaders
// (dof_prepare.comp, dof_gather.comp, dof_composite.comp), and the CPU tests.
// The shaders mirror these formulas by hand; keep them in sync.
//
// Conventions:
// - Scene depth is reversed Z (near = 1, far = 0, see DepthConvention.hpp);
//   the sky keeps the cleared depth 0 and so lies at the far plane.
// - View distance is the positive distance along the camera's forward axis,
//   in meters (world units), the distance a thin lens focuses at.
// - The circle of confusion (CoC) is a signed radius in pixels of the blurred
//   image: negative in front of the focus plane, positive behind it. Its
//   magnitude is the blur radius; the sign orders samples by depth.
namespace depth_of_field {

// Accepted settings ranges; deserialization rejects anything outside them.
inline constexpr float minimumFocusDistance = 0.05f;
inline constexpr float maximumFocusDistance = 10000.0f;
inline constexpr float minimumFStop = 0.5f;
inline constexpr float maximumFStop = 64.0f;
inline constexpr float minimumSensorHeightMillimeters = 1.0f;
inline constexpr float maximumSensorHeightMillimeters = 100.0f;
inline constexpr float minimumCocRadiusPixels = 1.0f;
inline constexpr float maximumCocRadiusPixels = 32.0f;

// Whether every parameter is finite and inside the ranges above.
bool validSettings(const DepthOfFieldSettings& settings);

// Projection terms the CoC needs, from a projection built like MeshPass builds
// it (glm::perspectiveRH_ZO with swapped planes, Y flipped, optional jitter).
// glm matrices are column-major: projection[column][row].
struct CameraTerms {
    float verticalFovRadians{0.0f};
    float depthScale{0.0f}; // projection[2][2]
    float depthBias{0.0f};  // projection[3][2]
};

CameraTerms cameraFromProjection(const glm::mat4& projection);

// View distance at a reversed-Z depth-buffer value: depthBias / (depth +
// depthScale). Depth 0 returns the far plane.
float viewDistanceFromDepth(float depth, const CameraTerms& camera);

// Focal length of a lens that covers verticalFovRadians on a sensor of the
// given height; both lengths in meters.
float focalLengthMeters(float verticalFovRadians, float sensorHeightMeters);

// Signed CoC radius at a view distance z for a thin lens of focal length f,
// f-number N, focused at S, imaged on a sensor of height h into H pixels:
//   radius(z) = clamp(scale * (1 - S / z), -maximumRadius, maximumRadius)
//   scale     = 0.5 * (f / N) * f / (S - f) * H / h
// The unclamped value is half the classic CoC diameter A * f * (z - S) /
// (z * (S - f)) with aperture diameter A = f / N, converted from sensor meters
// to pixels. A focus distance closer than 1.01 f is moved out to 1.01 f, where
// the lens can still focus.
struct CircleOfConfusion {
    float scale{0.0f};
    float focusDistance{1.0f};
    float maximumRadius{0.0f};

    float radiusPixels(float viewDistance) const;
};

CircleOfConfusion circleOfConfusion(const DepthOfFieldSettings& settings, float verticalFovRadians,
                                    float imageHeightPixels);

// Half-resolution extent the gather runs at: ceil(extent / 2).
glm::uvec2 gatherExtent(glm::uvec2 fullExtent);

// Radial step of the gather spiral, in half-resolution pixels at a radius of
// one pixel: each sample moves outward by spacing / radius, so samples keep a
// roughly constant density per area. Zero for Off.
float gatherRingSpacing(DepthOfFieldQuality quality);

// Samples the gather spiral takes (besides the center) for a maximum radius in
// half-resolution pixels; mirrors the loop in dof_gather.comp.
uint32_t gatherSampleCount(float maximumRadiusHalfPixels, float spacing);

// Upper bound on gatherSampleCount the shader enforces.
inline constexpr uint32_t maximumGatherSamples = 512;

// Weight of the half-resolution blur over the sharp scene in the composite,
// from the larger of the pixel's own CoC radius and the radius of the
// foreground blur that covers it (both in full-resolution pixels). Radii up to
// half a pixel stay sharp; two pixels and more take the blur.
float compositeBlend(float effectiveRadiusPixels);

} // namespace depth_of_field
