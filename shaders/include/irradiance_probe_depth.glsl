// Irradiance probe visibility from the probes' depth moments.
// Mirrors the irradiance_volume namespace in engine/graphics/IrradianceVolume.cpp;
// change both together. The includer declares the probe buffer as
// `irradianceVolume` with an `IrradianceProbe probes[]` member.
#ifndef ZENITH_IRRADIANCE_PROBE_DEPTH_GLSL
#define ZENITH_IRRADIANCE_PROBE_DEPTH_GLSL

#include "octahedral.glsl"

vec2 probeDepthTexel(uint probe, ivec2 texel) {
    texel = clamp(texel, ivec2(0), ivec2(IRRADIANCE_PROBE_DEPTH_RESOLUTION - 1));
    return irradianceVolume.probes[probe].depthMoments[texel.y * IRRADIANCE_PROBE_DEPTH_RESOLUTION + texel.x];
}

vec2 sampleProbeDepth(uint probe, vec3 direction) {
    vec2 texel = octahedralEncode(direction) * float(IRRADIANCE_PROBE_DEPTH_RESOLUTION) - 0.5;
    vec2 base = floor(texel);
    vec2 fraction = texel - base;
    ivec2 corner = ivec2(base);
    vec2 top = mix(probeDepthTexel(probe, corner), probeDepthTexel(probe, corner + ivec2(1, 0)), fraction.x);
    vec2 bottom =
        mix(probeDepthTexel(probe, corner + ivec2(0, 1)), probeDepthTexel(probe, corner + ivec2(1, 1)), fraction.x);
    return mix(top, bottom, fraction.y);
}

float chebyshevVisibility(vec2 moments, float distanceToProbe) {
    if (distanceToProbe <= moments.x) {
        return 1.0;
    }
    float variance = max(moments.y - moments.x * moments.x, 0.0001);
    float delta = distanceToProbe - moments.x;
    float probability = variance / (variance + delta * delta);
    return probability * probability * probability;
}

#endif
