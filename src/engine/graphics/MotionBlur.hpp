#pragma once

// Motion blur tile and reconstruction planning.

#include "Rendering.hpp"

#include <glm/glm.hpp>

#include <cstdint>

// Motion blur math shared by MotionBlurPass, its shaders
// (motion_blur_tile_max.comp, motion_blur_neighbor_max.comp, and
// motion_blur_reconstruct.comp), and the CPU tests.
//
// The pass follows McGuire et al., "A Reconstruction Filter for Plausible
// Motion Blur" (I3D 2012): the largest velocity of each square tile, then the
// largest of each tile's 3x3 neighbourhood, then a depth-aware gather along
// that neighbourhood velocity at every pixel whose neighbourhood moves.
//
// Units: motion vectors hold the unjittered UV displacement from the previous
// frame to the current one (see unjitteredMotionUv). The pass blurs a "colour
// region": the top-left part of the HDR scene image that holds the frame, in
// its own pixels. A blur velocity is the half-length of a pixel's blur in
// colour-region pixels, so a pixel spreads velocity pixels to either side.
namespace motion_blur {

// The exposure that shutterFraction is relative to; one 60 Hz frame.
inline constexpr float referenceFrameSeconds = 1.0f / 60.0f;
// Valid ranges of the RenderSettings parameters.
inline constexpr float minimumShutterFraction = 0.0f;
inline constexpr float maximumShutterFraction = 1.0f;
inline constexpr float minimumRadiusPixels = 1.0f;
inline constexpr float maximumRadiusPixels = 64.0f;
// Tile sizes are multiples of this many pixels (the tile shader's workgroup
// width), so the tile images are allocated for this smallest size.
inline constexpr uint32_t tileGranularity = 8;
// Frame times outside this range are clamped: shorter ones would amplify
// sub-pixel motion noise, and longer ones are hitches, not exposures.
inline constexpr float minimumFrameSeconds = 1.0f / 1000.0f;
inline constexpr float maximumFrameSeconds = 0.25f;

// Gather samples per blurred pixel; zero for Off.
uint32_t sampleCount(MotionBlurQuality quality);

// Whether stored parameters are inside their valid ranges.
bool validParameters(float shutterFraction, float maximumRadiusPixels);

// Multiplier from a frame's motion (which spans frameSeconds) to the motion
// during the exposure, shutterFraction * referenceFrameSeconds. The same
// on-screen speed therefore blurs equally at any frame rate. A non-positive
// frameSeconds (no previous frame) uses the reference frame time.
float velocityScale(float shutterFraction, float frameSeconds);

// The maximum radius scaled from output pixels to colour-region pixels, so
// dynamic resolution without temporal upsampling keeps the same on-screen
// length. Never below one pixel.
float regionRadiusPixels(float maximumRadiusOutputPixels, uint32_t regionWidth, uint32_t outputWidth);

// Tile edge in pixels: the radius rounded up to a multiple of
// tileGranularity, so the 3x3 neighbourhood of a pixel's tile covers every
// pixel whose blur can reach it.
uint32_t tileSize(float radiusPixels);

// Tiles covering a region; partial tiles at the right and bottom count.
glm::uvec2 tileCount(glm::uvec2 regionPixels, uint32_t tileSize);

// A pixel's blur velocity (half the exposure's displacement, in region
// pixels) from its motion vector, clamped to radiusPixels in length.
glm::vec2 blurVelocity(const glm::vec2& motionUv, const glm::uvec2& regionPixels, float velocityScale,
                       float radiusPixels);

// Everything one frame's dispatches need.
struct FramePlan {
    uint32_t samples{0};
    float velocityScale{0.0f};
    float radiusPixels{0.0f};
    uint32_t tileSize{tileGranularity};
    glm::uvec2 tiles{0u};
};

// Plans a frame for a colour region inside an output of outputWidth pixels.
// frameSeconds is the time since the previous frame (see velocityScale).
FramePlan planFrame(const RenderSettings& settings, glm::uvec2 regionPixels, uint32_t outputWidth, float frameSeconds);

} // namespace motion_blur
