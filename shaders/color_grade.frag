#version 450
#extension GL_GOOGLE_include_directive : require

// Display-referred finishing stage (ColorGradePass): the LUT grade, vignette
// and film grain, in that order, over the tone-mapped frame. Mirrors
// applyColorGrade in engine/graphics/ColorGrading.cpp; keep them in sync.

// Tone-mapped, display-linear frame written by post.frag.
layout(set = 0, binding = 0) uniform sampler2D displayImage;
// RGBA16F lattice of the .cube grade, sampled with linear filtering.
layout(set = 0, binding = 1) uniform sampler3D gradeLut;

layout(push_constant) uniform ColorGradeSettings {
    vec4 lutDomainMinimum; // xyz: domain minimum, w: LUT strength (0 skips the LUT)
    vec4 lutDomainMaximum; // xyz: domain maximum, w: lattice size
    vec4 vignette; // x: intensity (0 skips), y: smoothness, z: roundness
    vec4 filmGrain; // x: intensity (0 skips), y: cell size in pixels
    vec2 outputSize; // pixels; the render area covers the whole output
    uint frame; // seeds the grain pattern
    uint padding;
} settings;

layout(location = 0) out vec4 outColor;

#include "hash.glsl"

vec3 linearToSrgb(vec3 linear) {
    vec3 clamped = clamp(linear, 0.0, 1.0);
    return mix(1.055 * pow(clamped, vec3(1.0 / 2.4)) - 0.055, clamped * 12.92, lessThanEqual(clamped, vec3(0.0031308)));
}

vec3 srgbToLinear(vec3 encoded) {
    vec3 clamped = clamp(encoded, 0.0, 1.0);
    return mix(pow((clamped + 0.055) / 1.055, vec3(2.4)), clamped / 12.92, lessThanEqual(clamped, vec3(0.04045)));
}

vec3 sampleLut(vec3 encoded) {
    float size = settings.lutDomainMaximum.w;
    vec3 domainMinimum = settings.lutDomainMinimum.xyz;
    vec3 normalized = clamp((encoded - domainMinimum) / (settings.lutDomainMaximum.xyz - domainMinimum), 0.0, 1.0);
    return texture(gradeLut, normalized * ((size - 1.0) / size) + 0.5 / size).rgb;
}

float vignetteFactor(vec2 screenPosition) {
    float aspectRatio = settings.outputSize.x / settings.outputSize.y;
    vec2 scale = vec2(mix(1.0, aspectRatio, settings.vignette.z), 1.0);
    vec2 offset = (screenPosition - 0.5) * scale;
    float distanceToCenter = length(offset) / length(0.5 * scale);
    float falloff = smoothstep(1.0 - settings.vignette.y, 1.0, distanceToCenter);
    return 1.0 - settings.vignette.x * falloff;
}

float filmGrainNoise(uvec2 cell, uint frame) {
    uint hash = pcgHash(cell.x + pcgHash(cell.y + pcgHash(frame)));
    return float(hash) * (1.0 / 4294967296.0) - 0.5;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 extent = textureSize(displayImage, 0);
    vec3 color = clamp(texelFetch(displayImage, min(pixel, extent - 1), 0).rgb, 0.0, 1.0);
    if (settings.lutDomainMinimum.w > 0.0) {
        vec3 encoded = linearToSrgb(color);
        color = srgbToLinear(mix(encoded, sampleLut(encoded), settings.lutDomainMinimum.w));
    }
    if (settings.vignette.x > 0.0) {
        // gl_FragCoord is at pixel centres.
        color *= vignetteFactor(gl_FragCoord.xy / settings.outputSize);
    }
    if (settings.filmGrain.x > 0.0) {
        uvec2 cell = uvec2(floor(vec2(pixel) / settings.filmGrain.y));
        vec3 encoded = linearToSrgb(color) + settings.filmGrain.x * filmGrainNoise(cell, settings.frame);
        color = srgbToLinear(encoded);
    }
    outColor = vec4(color, 1.0);
}
