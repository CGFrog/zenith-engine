// Per-pixel reflection probe blending and the probe-weighted environment
// lookups for mesh.frag.
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_REFLECTION_PROBES_GLSL
#define ZENITH_MESH_REFLECTION_PROBES_GLSL

// Reflection probes blended at one fragment. The probe weights and
// environmentWeight sum to one; mirrors ReflectionProbeShading.
struct ProbeBlend {
    uint count;
    uint records[MAXIMUM_BLENDED_REFLECTION_PROBES];
    float weights[MAXIMUM_BLENDED_REFLECTION_PROBES];
    float environmentWeight;
};

// Mirrors shadeReflectionProbes in ReflectionProbes.cpp: the cluster's probes
// that reach the fragment, highest priority first, then highest influence,
// then lowest record index; their influences are normalized and scaled by the
// largest influence, and the scene environment fills the remainder.
ProbeBlend blendReflectionProbes(vec3 position) {
    ProbeBlend blend;
    blend.count = 0u;
    blend.environmentWeight = 1.0;
    if (camera.reflectionProbeParams.x < 0.5) {
        return blend;
    }
    float influences[MAXIMUM_RESIDENT_REFLECTION_PROBES];
    uint reaching = 0u;
    uint remaining = reflectionProbes.clusterMasks[fragmentCluster()];
    while (remaining != 0u) {
        uint probe = uint(findLSB(remaining));
        remaining &= remaining - 1u;
        vec4 centerBlend = reflectionProbes.records[probe].centerBlend;
        vec3 outside = max(abs(position - centerBlend.xyz) - reflectionProbes.records[probe].halfExtents.xyz,
                           vec3(0.0));
        float distanceOutside = length(outside);
        if (distanceOutside <= centerBlend.w) {
            influences[probe] = centerBlend.w <= 0.0 ? 1.0 : 1.0 - distanceOutside / centerBlend.w;
            reaching |= 1u << probe;
        }
    }
    float total = 0.0;
    float coverage = 0.0;
    while (reaching != 0u && blend.count < MAXIMUM_BLENDED_REFLECTION_PROBES) {
        // Visiting ascending indices and replacing only on a strict win keeps
        // the lowest index on ties.
        uint best = uint(findLSB(reaching));
        remaining = reaching & (reaching - 1u);
        while (remaining != 0u) {
            uint probe = uint(findLSB(remaining));
            remaining &= remaining - 1u;
            int priority = reflectionProbes.records[probe].parameters.x;
            int bestPriority = reflectionProbes.records[best].parameters.x;
            if (priority > bestPriority || (priority == bestPriority && influences[probe] > influences[best])) {
                best = probe;
            }
        }
        reaching &= ~(1u << best);
        blend.records[blend.count] = best;
        blend.weights[blend.count] = influences[best];
        total += influences[best];
        coverage = max(coverage, influences[best]);
        ++blend.count;
    }
    for (uint index = 0u; index < blend.count; ++index) {
        blend.weights[index] = total > 0.0 ? blend.weights[index] / total * coverage : 0.0;
    }
    blend.environmentWeight = 1.0 - coverage;
    return blend;
}

// Parallax-corrected lookup direction into a probe captured at its box
// center; mirrors boxProjectReflection in ReflectionProbes.cpp.
vec3 boxProjectReflection(vec3 position, vec3 direction, uint record) {
    vec3 center = reflectionProbes.records[record].centerBlend.xyz;
    vec3 halfExtents = reflectionProbes.records[record].halfExtents.xyz;
    vec3 minimum = center - halfExtents;
    vec3 maximum = center + halfExtents;
    vec3 origin = clamp(position, minimum, maximum);
    vec3 safeDirection = direction;
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(safeDirection[axis]) < 1.0e-6) {
            safeDirection[axis] = safeDirection[axis] < 0.0 ? -1.0e-6 : 1.0e-6;
        }
    }
    vec3 farSlab = max((maximum - origin) / safeDirection, (minimum - origin) / safeDirection);
    float exitDistance = min(min(farSlab.x, farSlab.y), farSlab.z);
    vec3 projected = origin + direction * exitDistance - center;
    float projectedLength = length(projected);
    return projectedLength > 0.0 ? projected / projectedLength : direction;
}

vec3 probeIrradianceAt(uint record, vec3 normal) {
    vec4 coefficients[9];
    for (uint index = 0u; index < 9u; ++index) {
        coefficients[index] = reflectionProbes.irradiance[record * 9u + index];
    }
    return evaluateIrradiance(coefficients, normal);
}

// Lambertian outgoing radiance of a white surface lit by the environment and
// the fragment's blended probes.
vec3 environmentIrradianceAt(vec3 normal, ProbeBlend blend) {
    vec3 irradiance = vec3(0.0);
    if (blend.environmentWeight > 0.0) {
        irradiance = evaluateIrradiance(environmentIrradiance[0].coefficients, normal);
        float probeWeight = camera.environmentParams.y;
        if (probeWeight > 0.0) {
            irradiance =
                mix(irradiance, evaluateIrradiance(environmentIrradiance[1].coefficients, normal), probeWeight);
        }
        irradiance *= blend.environmentWeight;
    }
    for (uint index = 0u; index < blend.count; ++index) {
        irradiance += probeIrradianceAt(blend.records[index], normal) * blend.weights[index];
    }
    return irradiance;
}

vec3 prefilteredRadiance(vec3 position, vec3 direction, float roughness, ProbeBlend blend) {
    float lod = roughness * camera.environmentParams.z;
    vec3 radiance = vec3(0.0);
    if (blend.environmentWeight > 0.0) {
        radiance = textureLod(prefilteredEnvironment[0], direction, lod).rgb;
        float probeWeight = camera.environmentParams.y;
        if (probeWeight > 0.0) {
            radiance = mix(radiance, textureLod(prefilteredEnvironment[1], direction, lod).rgb, probeWeight);
        }
        radiance *= blend.environmentWeight;
    }
    for (uint index = 0u; index < blend.count; ++index) {
        uint record = blend.records[index];
        vec3 projected = boxProjectReflection(position, direction, record);
        float layer = float(reflectionProbes.records[record].parameters.y);
        radiance += textureLod(reflectionProbeSpecular, vec4(projected, layer), lod).rgb * blend.weights[index];
    }
    return radiance;
}

#endif
