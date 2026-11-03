// One irradiance volume probe (U6).
// Mirrors irradiance_volume::GpuProbe in engine/graphics/IrradianceVolume.hpp
// (std430). irradiance holds order-2 SH of irradiance / pi (irradiance[0].w is
// 1 for valid probes); depthMoments the mean and mean-square distance to the
// nearest surface per octahedral texel.
#ifndef ZENITH_IRRADIANCE_VOLUME_GLSL
#define ZENITH_IRRADIANCE_VOLUME_GLSL

const int IRRADIANCE_PROBE_DEPTH_RESOLUTION = 8;
struct IrradianceProbe {
    vec4 irradiance[9];
    vec2 depthMoments[IRRADIANCE_PROBE_DEPTH_RESOLUTION * IRRADIANCE_PROBE_DEPTH_RESOLUTION];
};

#endif
