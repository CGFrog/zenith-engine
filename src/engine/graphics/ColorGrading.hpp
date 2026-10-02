#pragma once

#include "Rendering.hpp"
#include "engine/assets/ColorLut.hpp"

#include <cstdint>
#include <glm/glm.hpp>

// CPU reference for the display-referred finishing stage evaluated on the GPU
// by color_grade.frag (ColorGradePass). GLSL has no include support, so the
// shader mirrors these functions by hand; keep them in sync.
//
// The stage runs after tone mapping on display-linear colour in [0, 1] (the
// output target is sRGB, so the hardware encodes on store). The LUT and film
// grain operate on sRGB-encoded values, the encoding .cube grades are authored
// for; the vignette scales display-linear colour.
namespace color_grading {
// Whether each effect contributes; ColorGradePass records no work when none
// does (and skips the LUT when its file is unavailable).
bool lutRequested(const ColorLutSettings& settings);
bool vignetteActive(const VignetteSettings& settings);
bool filmGrainActive(const FilmGrainSettings& settings);

// The IEC 61966-2-1 sRGB transfer functions, per channel, for values in [0, 1].
glm::vec3 linearToSrgb(const glm::vec3& linear);
glm::vec3 srgbToLinear(const glm::vec3& encoded);

// Trilinear lookup of an encoded colour, clamped to the table's domain. The
// GPU samples a size^3 texture at lutTextureCoordinate with linear filtering,
// which is the same interpolation.
glm::vec3 sampleColorLut(const ColorLut& lut, const glm::vec3& encoded);
// Normalized 3D texture coordinate of an encoded colour: the domain maps onto
// the centres of the first and last texels.
glm::vec3 lutTextureCoordinate(const glm::vec3& encoded, const glm::vec3& domainMinimum, const glm::vec3& domainMaximum,
                               uint32_t size);

// Multiplier for display-linear colour at a normalized screen position (0..1,
// either origin): 1 at the centre, (1 - intensity) at the corners.
// aspectRatio is output width over height.
float vignette(const glm::vec2& screenPosition, float aspectRatio, const VignetteSettings& settings);

// Uniform grain value in [-0.5, 0.5) for a grain cell (output pixel divided
// by the grain size, floored) and frame serial. Integer hashing keeps it
// identical on CPU and GPU and repeatable for a given frame.
float filmGrainNoise(const glm::uvec2& cell, uint32_t frame);

// The whole stage for one pixel: LUT, vignette, then grain. `lut` may be null
// (no LUT applied). pixel is the output pixel coordinate and extent the output
// size in pixels.
glm::vec3 applyColorGrade(const glm::vec3& displayLinear, const glm::uvec2& pixel, const glm::uvec2& extent,
                          uint32_t frame, const ColorLut* lut, const RenderSettings& settings);
} // namespace color_grading
