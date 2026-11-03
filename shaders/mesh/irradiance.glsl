// Diffuse irradiance for mesh.frag: SH evaluation and the irradiance volume
// faded into the environment term.
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_IRRADIANCE_GLSL
#define ZENITH_MESH_IRRADIANCE_GLSL

vec3 evaluateIrradiance(vec4 coefficients[9], vec3 normal) {
    vec3 result = coefficients[0].rgb * 0.282095 + coefficients[1].rgb * (0.488603 * normal.y) +
                  coefficients[2].rgb * (0.488603 * normal.z) + coefficients[3].rgb * (0.488603 * normal.x) +
                  coefficients[4].rgb * (1.092548 * normal.x * normal.y) +
                  coefficients[5].rgb * (1.092548 * normal.y * normal.z) +
                  coefficients[6].rgb * (0.315392 * (3.0 * normal.z * normal.z - 1.0)) +
                  coefficients[7].rgb * (1.092548 * normal.x * normal.z) +
                  coefficients[8].rgb * (0.546274 * (normal.x * normal.x - normal.y * normal.y));
    return max(result, vec3(0.0));
}

#include "irradiance_probe_depth.glsl"

// Visibility-weighted trilinear blend of the eight probes around the point.
// Returns irradiance / pi in rgb and the volume's edge weight in a (zero when
// no probe sees the point).
vec4 sampleIrradianceVolume(vec3 position, vec3 normal, vec3 viewDirection) {
    uvec3 counts = camera.irradianceVolumeCounts.xyz;
    vec3 minimum = camera.irradianceVolumeMinimum.xyz;
    vec3 cellSize = camera.irradianceVolumeCellSize.xyz;
    // The edge weight comes first so fragments outside the box skip the probes;
    // the CPU reference still evaluates them there for the baker's bounces.
    vec3 maximum = minimum + cellSize * vec3(counts);
    vec3 inside = min(position - minimum, maximum - position);
    float edgeDistance = min(inside.x, min(inside.y, inside.z));
    if (edgeDistance < 0.0) {
        return vec4(0.0);
    }
    float blendDistance = camera.irradianceVolumeCellSize.w;
    float edge = blendDistance <= 0.0 ? 1.0 : min(edgeDistance / blendDistance, 1.0);
    vec3 biased = position + normal * camera.irradianceVolumeBias.x + viewDirection * camera.irradianceVolumeBias.y;
    vec3 lastProbe = vec3(counts) - 1.0;
    vec3 lattice = clamp((biased - minimum) / cellSize - 0.5, vec3(0.0), lastProbe);
    vec3 base = min(floor(lattice), max(lastProbe - 1.0, vec3(0.0)));
    vec3 alpha = clamp(lattice - base, vec3(0.0), vec3(1.0));
    vec3 irradiance = vec3(0.0);
    float totalWeight = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner) {
        uvec3 offset = uvec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        vec3 trilinearAxes = mix(1.0 - alpha, alpha, vec3(offset));
        float trilinear = trilinearAxes.x * trilinearAxes.y * trilinearAxes.z;
        uvec3 index = min(uvec3(base) + offset, counts - 1u);
        uint probe = index.x + counts.x * (index.y + counts.y * index.z);
        if (irradianceVolume.probes[probe].irradiance[0].w < 0.5 || trilinear <= 0.0) {
            continue;
        }
        vec3 probePosition = minimum + (vec3(index) + 0.5) * cellSize;
        vec3 toProbe = probePosition - position;
        float probeDistance = length(toProbe);
        vec3 directionToProbe = probeDistance > 0.0001 ? toProbe / probeDistance : normal;
        float facing = (dot(directionToProbe, normal) + 1.0) * 0.5;
        float weight = facing * facing + 0.2;
        vec3 probeToPoint = biased - probePosition;
        float pointDistance = length(probeToPoint);
        vec3 direction = pointDistance > 0.0001 ? probeToPoint / pointDistance : normal;
        weight *= chebyshevVisibility(sampleProbeDepth(probe, direction), pointDistance);
        weight = max(weight, 0.000001);
        if (weight < 0.2) {
            weight *= weight * weight / 0.04;
        }
        weight *= trilinear;
        irradiance += evaluateIrradiance(irradianceVolume.probes[probe].irradiance, normal) * weight;
        totalWeight += weight;
    }
    if (totalWeight <= 0.0) {
        return vec4(0.0);
    }
    return vec4(irradiance / totalWeight, edge);
}

// Indirect diffuse radiance of a white surface: the bound irradiance volume
// where it applies, faded into the environment term (`environment`, already
// scaled by the environment's diffuse intensity) across the volume's edge.
vec3 indirectDiffuseRadiance(vec3 environment, vec3 position, vec3 normal, vec3 viewDirection) {
    if (camera.irradianceVolumeCounts.w == 0u) {
        return environment;
    }
    vec4 volume = sampleIrradianceVolume(position, normal, viewDirection);
    return mix(environment, volume.rgb * camera.irradianceVolumeMinimum.w, volume.a);
}

#endif
