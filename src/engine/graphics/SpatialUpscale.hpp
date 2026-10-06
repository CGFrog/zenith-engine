#pragma once

// CPU side of the upscalers: render-scale presets, dynamic resolution, and AMD FSR 1 constants and reference math.

#include "Rendering.hpp"

#include <glm/glm.hpp>

// CPU side of the upscalers: render-scale presets, the dynamic-resolution
// range a frame follows, and the constants and reference math of AMD FidelityFX
// Super Resolution 1 (EASU and RCAS). The reference functions mirror
// shaders/fsr_easu.comp and shaders/fsr_rcas.frag; keep them in sync.
//
// Color space: FSR 1 filters perceptual values. The frame is tone mapped at
// render resolution, EASU encodes those display-linear values with gamma 2
// (sqrt) before filtering and writes them gamma-2 encoded, and RCAS sharpens
// that encoding before decoding (squaring) for the output. Every reference
// function below therefore takes and returns gamma-2 encoded colors in [0, 1].

// Output-to-render ratio per axis for a preset (1.5, 1.7 or 2).
float upscalerScaleFactor(UpscalerQuality quality);

// Render-to-output ratio per axis the upscaler asks for: the reciprocal of the
// preset's factor, or 1 when the mode is Off.
float upscalerRenderScale(const UpscalerSettings& upscaler);

// The dynamic-resolution settings a frame's internal extent follows, given the
// upscaler that will actually run (pass Off when the requested one cannot, so
// the frame renders natively). Off returns resolution unchanged. An upscaler fixes the scale at
// its preset when dynamic resolution is disabled; when it is enabled, its
// scale range applies relative to the preset (both bounds are multiplied by
// the preset's render scale), so the preset is the unloaded resolution.
DynamicResolutionSettings upscaledDynamicResolution(const DynamicResolutionSettings& resolution,
                                                    const UpscalerSettings& upscaler);

// Maps output pixels to the input viewport: an output pixel index p samples
// input pixel position p * scale + offset, where integer positions are input
// texel centers. Matches FsrEasuCon's con0. The input viewport is the top-left
// rendered region of a possibly larger input image.
struct EasuConstants {
    glm::vec2 scale{1.0f};
    glm::vec2 offset{0.0f};
    // Last texel inside the input viewport; taps clamp to it.
    glm::ivec2 inputMaximum{0};
    glm::ivec2 outputSize{1};
};

EasuConstants easuConstants(glm::uvec2 inputViewport, glm::uvec2 outputSize);

// The 12 taps around an output pixel, at offsets from the input texel f that
// lies at floor(p * scale + offset):
//      b c
//    e f g h
//    i j k l
//      n o
struct EasuTaps {
    glm::vec3 b, c, e, f, g, h, i, j, k, l, n, o;
};

// EASU's edge-adaptive Lanczos-2 reconstruction at the given fractional
// position inside the f-g-j-k quad, deringed to the range of those four taps.
glm::vec3 easuReconstruct(const EasuTaps& taps, glm::vec2 fraction);

// RCAS sharpening attenuation in stops (0 is maximum sharpening).
float rcasSharpnessStops(float sharpness);
// Multiplier on RCAS's negative lobe: 2^-stops (FsrRcasCon's con.x).
float rcasLobeScale(float sharpness);

// Robust contrast-adaptive sharpening of one pixel from its 4-neighborhood,
// with RCAS's noise attenuation. Its lobe is limited so no channel leaves
// [0, 1], and flat neighborhoods pass through unchanged.
glm::vec3 rcasSharpen(const glm::vec3& north, const glm::vec3& west, const glm::vec3& center, const glm::vec3& east,
                      const glm::vec3& south, float lobeScale);
