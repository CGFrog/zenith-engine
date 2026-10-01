#pragma once

#include "Rendering.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <span>

// CPU reference for the display transform and exposure math evaluated on the GPU
// by post.frag, luminance_histogram.comp and exposure_adapt.comp. GLSL has no
// include support, so the shaders mirror these functions by hand; keep them in
// sync.

inline constexpr uint32_t luminanceHistogramBinCount = 256;
// Scene luminance that auto exposure maps to after compensation.
inline constexpr float exposureMiddleGrey = 0.18f;

float relativeLuminance(const glm::vec3& linearColor);

// Maps linear scene radiance (already exposed) to display-linear values in
// [0, 1]. The swapchain uses an sRGB format, so the hardware applies the sRGB
// encoding on store; AgX's display-encoded output is therefore decoded back to
// linear with its 2.2 reference exponent.
glm::vec3 toneMap(const glm::vec3& exposedColor, TonemapOperator tonemapOperator);

// Histogram bin for a linear luminance. Values at or below 2^minimum land in
// bin 0 and values at or above 2^maximum land in the last bin.
uint32_t luminanceHistogramBin(float luminance, float minimumLogLuminance, float maximumLogLuminance);

// Average log2 luminance of the pixels between the low and high percentiles of
// the histogram, using each bin's center. Empty histograms have no average.
std::optional<float> histogramAverageLogLuminance(std::span<const uint32_t> bins, const ExposureSettings& settings);

// Exposure multiplier that maps the average luminance to middle grey, scaled by
// the compensation.
float autoExposureTarget(float averageLogLuminance, float compensationEv);

float manualExposure(const ExposureSettings& settings);

// Frame-rate independent exponential approach toward the target in log2 space.
float adaptExposure(float previousExposure, float targetExposure, float deltaSeconds, const ExposureSettings& settings);
