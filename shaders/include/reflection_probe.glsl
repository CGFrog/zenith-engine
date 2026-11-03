// One resident reflection probe and the probe limits.
// Mirrors GpuReflectionProbe in engine/graphics/passes/ReflectionProbeClusterPass.hpp
// (std430), and maximumResidentReflectionProbes and
// maximumBlendedReflectionProbes in engine/graphics/ReflectionProbes.hpp.
#ifndef ZENITH_REFLECTION_PROBE_GLSL
#define ZENITH_REFLECTION_PROBE_GLSL

const uint MAXIMUM_RESIDENT_REFLECTION_PROBES = 8;
const uint MAXIMUM_BLENDED_REFLECTION_PROBES = 4;

struct ReflectionProbeRecord {
    vec4 centerBlend; // world box center, blend distance
    vec4 halfExtents; // world box half extents
    ivec4 parameters; // x: priority, y: probe cache slot
    vec4 viewMinimum; // view-space influence bounds, used by the cluster pass
    vec4 viewMaximum;
};

#endif
