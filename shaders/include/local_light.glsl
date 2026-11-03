// One clustered local light.
// Mirrors GpuLocalLight in engine/graphics/passes/ClusteredLightingPass.hpp
// (std430).
#ifndef ZENITH_LOCAL_LIGHT_GLSL
#define ZENITH_LOCAL_LIGHT_GLSL

struct LocalLight {
    vec4 positionRange;  // world position, range
    vec4 colorIntensity; // linear color, intensity
    vec4 directionType;  // world direction, type (0 point, 1 spot)
    vec4 spotCone;       // inner and outer cone cosines, first local shadow tile (-1: unshadowed)
};

#endif
