#version 450
#extension GL_EXT_nonuniform_qualifier : require

const float PI = 3.14159265359;

// Quality variants baked in by MeshPass::meshPipelines so disabled paths compile out.
layout(constant_id = 0) const bool PHYSICALLY_BASED_LIGHTING = true;
layout(constant_id = 1) const int SHADOW_CASCADE_COUNT = 3;

layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 lightViewProjections[3];
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 environmentSky;
    vec4 environmentGround;
    vec4 shadowSettings;
    vec4 cascadeSplits;
    vec4 renderingSettings;
    vec4 environmentSpecular[6];
    // Clustered local lighting; mirrors CameraData in MeshPass.cpp.
    vec4 clusterDepth;     // xy: slice log scale, bias; zw: view depth = z / (depth + w)
    vec4 clusterTileScale; // xy: cluster columns, rows per pixel
    uvec4 clusterGrid;     // columns, rows, depth slices, local light count
    // x: image-based lighting, y: probe weight, z: specular mip of roughness 1,
    // w: ambient-occlusion texels per render pixel
    vec4 environmentParams;
    // Irradiance volume (U6); mirrors CameraData in MeshPass.cpp.
    vec4 irradianceVolumeMinimum;  // xyz: world minimum corner, w: intensity
    vec4 irradianceVolumeCellSize; // xyz: world probe spacing, w: edge blend distance
    uvec4 irradianceVolumeCounts;  // xyz: probes per axis, w: 1 when a volume is bound
    vec4 irradianceVolumeBias;     // x: normal bias, y: view bias
    // Temporal resolve: NDC jitter applied to the current (xy) and previous (zw)
    // projections, removed from motion vectors. Keep last in the block.
    vec4 temporalJitter;
} camera;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;

// Image-based lighting from EnvironmentPass. Slot 0 is the scene environment and
// slot 1 the dominant reflection probe, mixed by camera.environmentParams.y.
layout(set = 0, binding = 5) uniform samplerCube prefilteredEnvironment[2];
// Order-2 spherical harmonics of irradiance / pi (see environment_lighting::projectIrradiance).
layout(std430, set = 0, binding = 6) readonly buffer EnvironmentIrradiance {
    vec4 coefficients[9];
} environmentIrradiance[2];
// Split-sum BRDF scale and bias, indexed by (n.v, roughness).
layout(set = 0, binding = 7) uniform sampler2D environmentBrdf;

// Irradiance volume (U6): mirrors irradiance_volume::GpuProbe (std430). Same SH
// convention as EnvironmentIrradiance; irradiance[0].w is 1 for valid probes.
// depthMoments holds the mean and mean-square distance to the nearest surface
// per octahedral texel (irradianceProbeDepthResolution squared texels).
const int IRRADIANCE_PROBE_DEPTH_RESOLUTION = 8;
struct IrradianceProbe {
    vec4 irradiance[9];
    vec2 depthMoments[IRRADIANCE_PROBE_DEPTH_RESOLUTION * IRRADIANCE_PROBE_DEPTH_RESOLUTION];
};
layout(std430, set = 0, binding = 8) readonly buffer IrradianceVolumeProbes {
    IrradianceProbe probes[];
} irradianceVolume;
// GTAO visibility of the opaque scene (AmbientOcclusionPass); uniformly 1 when
// ambient occlusion is off or the depth prepass did not run.
layout(set = 0, binding = 13) uniform sampler2D screenSpaceOcclusion;

// Mirrors MaterialRecord in MeshPass.cpp (std430).
struct MaterialRecord {
    vec4 baseColor;
    vec4 factors;  // metallic, roughness, normal scale, occlusion strength
    vec4 emissive; // rgb radiance; w unused
    vec4 surface;  // alpha cutoff, alpha mode (0 opaque, 1 mask, 2 blend)
    uvec4 textures; // bindless slots: base color, normal, metallic-roughness, occlusion
    uvec4 extraTextures; // bindless slots: emissive
};

layout(std430, set = 0, binding = 4) readonly buffer Materials {
    MaterialRecord values[];
} materials;

// Mirrors GpuLocalLight in ClusteredLightingPass.hpp (std430).
struct LocalLight {
    vec4 positionRange;  // world position, range
    vec4 colorIntensity; // linear color, intensity
    vec4 directionType;  // world direction, type (0 point, 1 spot)
    vec4 spotCone;       // inner and outer cone cosines
};

layout(std430, set = 0, binding = 9) readonly buffer LocalLights {
    LocalLight values[];
} localLights;

// Written by light_clusters.comp: per cluster, x = first list entry, y = count.
layout(std430, set = 0, binding = 10) readonly buffer ClusterRanges {
    uvec2 values[];
} clusterRanges;

layout(std430, set = 0, binding = 11) readonly buffer ClusterLightIndices {
    uint values[];
} clusterLightIndices;

// Bindless material textures; slots come from MaterialRecord.
layout(set = 1, binding = 0) uniform sampler materialSampler;
layout(set = 1, binding = 1) uniform texture2D materialTextures[];

vec4 sampleMaterialTexture(uint slot, vec2 uv) {
    return texture(sampler2D(materialTextures[nonuniformEXT(slot)], materialSampler), uv);
}

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 texCoord;
layout(location = 3) in vec4 worldTangent;
layout(location = 4) in vec4 currentClipPosition;
layout(location = 5) in vec4 previousClipPosition;
layout(location = 6) flat in uint materialIndex;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outMotionReactive;

float distributionGGX(vec3 normal, vec3 halfVector, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denominator * denominator, 0.0001);
}

float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

vec3 fresnelSchlick(float cosine, vec3 reflectance) {
    return reflectance + (1.0 - reflectance) * pow(1.0 - cosine, 5.0);
}

vec3 fresnelSchlickRoughness(float cosine, vec3 reflectance, float roughness) {
    return reflectance + (max(vec3(1.0 - roughness), reflectance) - reflectance) * pow(1.0 - cosine, 5.0);
}

float sampleShadowCascade(vec3 position, vec3 normal, vec3 lightDirection, int cascade) {
    float normalOffset = camera.shadowSettings.x * (1.0 - max(dot(normal, lightDirection), 0.0));
    vec4 clip = camera.lightViewProjections[cascade] * vec4(position + normal * normalOffset, 1.0);
    vec3 projected = clip.xyz / clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0 || any(lessThan(uv, vec2(0.0))) ||
        any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    const vec2 poissonDisk[16] = vec2[16](
        vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
        vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
        vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
        vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
        vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
        vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
        vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
        vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));
    float angle = fract(sin(dot(position.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.28318530718;
    mat2 rotation = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
    const int sampleCount = SHADOW_CASCADE_COUNT > 2 ? 16 : SHADOW_CASCADE_COUNT > 1 ? 8 : 4;
    float visibility = 0.0;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        vec2 offset = rotation * poissonDisk[sampleIndex] * texel * 1.75;
        visibility += texture(shadowMap, vec4(uv + offset, float(cascade), projected.z));
    }
    return visibility / float(sampleCount);
}

int shadowCascade(vec3 position) {
    float cameraDistance = length(position - camera.cameraPosition.xyz);
    int cascade = 0;
    if (SHADOW_CASCADE_COUNT > 1 && cameraDistance > camera.cascadeSplits.x) {
        cascade = 1;
    }
    if (SHADOW_CASCADE_COUNT > 2 && cameraDistance > camera.cascadeSplits.y) {
        cascade = 2;
    }
    return cascade;
}

float shadowVisibility(vec3 position, vec3 normal, vec3 lightDirection) {
    if (SHADOW_CASCADE_COUNT == 0 || camera.shadowSettings.y < 0.5) {
        return 1.0;
    }
    float cameraDistance = length(position - camera.cameraPosition.xyz);
    int cascade = shadowCascade(position);
    float visibility = sampleShadowCascade(position, normal, lightDirection, cascade);
    const int cascadeCount = SHADOW_CASCADE_COUNT;
    if (cascade + 1 < cascadeCount) {
        float splitNear = cascade == 0 ? 0.0 : camera.cascadeSplits[cascade - 1];
        float splitFar = camera.cascadeSplits[cascade];
        float blendWidth = max((splitFar - splitNear) * camera.shadowSettings.w, 0.001);
        float blend = smoothstep(splitFar - blendWidth, splitFar, cameraDistance);
        if (blend > 0.0) {
            visibility = mix(visibility, sampleShadowCascade(position, normal, lightDirection, cascade + 1), blend);
        }
    } else if (cascadeCount > 1) {
        float lastSplit = camera.cascadeSplits[cascadeCount - 1];
        float fadeWidth = max(lastSplit * 0.1, 0.001);
        visibility = mix(visibility, 1.0, smoothstep(lastSplit - fadeWidth, lastSplit, cameraDistance));
    }
    return visibility;
}

// Reflected fraction of one light's irradiance, excluding radiance and the cosine
// term. The fast variant is unnormalized Lambert, matching its tuned brightness.
vec3 directResponse(vec3 normal, vec3 viewDirection, vec3 lightDirection, float nDotV, float nDotL, vec3 albedo,
                    float metallic, float roughness, vec3 reflectance, float viewGeometry) {
    if (!PHYSICALLY_BASED_LIGHTING) {
        return albedo;
    }
    vec3 halfVector = normalize(viewDirection + lightDirection);
    vec3 fresnel = fresnelSchlick(max(dot(halfVector, viewDirection), 0.0), reflectance);
    float distribution = distributionGGX(normal, halfVector, roughness);
    float geometry = viewGeometry * geometrySchlickGGX(nDotL, roughness);
    vec3 specular = distribution * geometry * fresnel / max(4.0 * nDotV * nDotL, 0.0001);
    vec3 diffuse = (vec3(1.0) - fresnel) * (1.0 - metallic) * albedo / PI;
    return diffuse + specular;
}

// Cluster of the current fragment; mirrors ClusterGrid::clusterIndexAt.
uint fragmentCluster() {
    uvec2 tile = min(uvec2(gl_FragCoord.xy * camera.clusterTileScale.xy), camera.clusterGrid.xy - 1u);
    float viewDepth = camera.clusterDepth.z / (gl_FragCoord.z + camera.clusterDepth.w);
    float slice = log(viewDepth) * camera.clusterDepth.x + camera.clusterDepth.y;
    uint depthSlice = min(uint(max(slice, 0.0)), camera.clusterGrid.z - 1u);
    return tile.x + camera.clusterGrid.x * (tile.y + camera.clusterGrid.y * depthSlice);
}

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

// Irradiance volume (U6). Mirrors the irradiance_volume namespace in
// engine/graphics/IrradianceVolume.cpp; change both together.
vec2 signNotZero(vec2 value) {
    return vec2(value.x >= 0.0 ? 1.0 : -1.0, value.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octahedralEncode(vec3 direction) {
    vec2 projected = direction.xz / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    if (direction.y < 0.0) {
        projected = (1.0 - abs(projected.yx)) * signNotZero(projected);
    }
    return projected * 0.5 + 0.5;
}

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

// Lambertian outgoing radiance of a white surface lit by the environment.
vec3 environmentIrradianceAt(vec3 normal) {
    vec3 irradiance = evaluateIrradiance(environmentIrradiance[0].coefficients, normal);
    float probeWeight = camera.environmentParams.y;
    if (probeWeight > 0.0) {
        irradiance = mix(irradiance, evaluateIrradiance(environmentIrradiance[1].coefficients, normal), probeWeight);
    }
    return irradiance;
}

vec3 prefilteredRadiance(vec3 direction, float roughness) {
    float lod = roughness * camera.environmentParams.z;
    vec3 radiance = textureLod(prefilteredEnvironment[0], direction, lod).rgb;
    float probeWeight = camera.environmentParams.y;
    if (probeWeight > 0.0) {
        radiance = mix(radiance, textureLod(prefilteredEnvironment[1], direction, lod).rgb, probeWeight);
    }
    return radiance;
}

// Lagarde's specular occlusion from ambient occlusion: 1 when unoccluded, 0
// when fully occluded. Mirrors ambient_occlusion::specularOcclusion.
float specularOcclusion(float nDotV, float occlusion, float roughness) {
    return clamp(pow(nDotV + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion, 0.0, 1.0);
}

// Screen-space ambient occlusion of the opaque surface at this fragment. The
// occlusion texel t was evaluated at render pixel center t / scale + 0.5, so
// fragment f samples texel coordinate f * scale + 0.5 * (1 - scale); mirrors
// ambient_occlusion::occlusionUv.
float screenSpaceAmbientOcclusion() {
    float scale = camera.environmentParams.w;
    vec2 uv = (gl_FragCoord.xy * scale + 0.5 * (1.0 - scale)) / vec2(textureSize(screenSpaceOcclusion, 0));
    return textureLod(screenSpaceOcclusion, uv, 0.0).r;
}

// Flat sky/ground approximation used when image-based lighting is disabled.
// Occlusion applies to the diffuse term and, as specular occlusion, to the
// reflection term.
vec3 hemisphereLighting(vec3 position, vec3 normal, vec3 viewDirection, float nDotV, vec3 albedo, float metallic,
                        float roughness, vec3 reflectance, float occlusion) {
    float hemisphereWeight = normal.y * 0.5 + 0.5;
    vec3 diffuseEnvironment = mix(camera.environmentGround.rgb, camera.environmentSky.rgb, hemisphereWeight);
    vec3 indirect = albedo * indirectDiffuseRadiance(diffuseEnvironment * camera.environmentSky.w, position, normal,
                                                     viewDirection);
    if (!PHYSICALLY_BASED_LIGHTING) {
        return indirect * occlusion;
    }
    vec3 indirectFresnel = fresnelSchlickRoughness(nDotV, reflectance, roughness);
    vec3 indirectDiffuse = (vec3(1.0) - indirectFresnel) * (1.0 - metallic) * indirect;
    float specularLevel = roughness * 5.0;
    int specularLow = int(floor(specularLevel));
    int specularHigh = min(specularLow + 1, 5);
    vec3 specularEnvironment = mix(camera.environmentSpecular[specularLow].rgb,
                                   camera.environmentSpecular[specularHigh].rgb, fract(specularLevel));
    vec3 indirectSpecular = indirectFresnel * specularEnvironment * camera.environmentGround.w *
                            mix(1.0, 0.35, roughness);
    return indirectDiffuse * occlusion + indirectSpecular * specularOcclusion(nDotV, occlusion, roughness);
}

// Split-sum image-based lighting with ambient occlusion applied to the diffuse
// term and converted to specular occlusion for reflections.
vec3 imageBasedLighting(vec3 position, vec3 normal, vec3 geometricNormal, vec3 viewDirection, float nDotV,
                        vec3 albedo, float metallic, float roughness, vec3 reflectance, float occlusion) {
    vec3 diffuse = albedo * indirectDiffuseRadiance(environmentIrradianceAt(normal) * camera.environmentSky.w, position,
                                                    normal, viewDirection);
    if (!PHYSICALLY_BASED_LIGHTING) {
        return diffuse * occlusion;
    }
    vec3 diffuseWeight = (vec3(1.0) - fresnelSchlickRoughness(nDotV, reflectance, roughness)) * (1.0 - metallic);
    vec3 reflected = reflect(-viewDirection, normal);
    vec2 brdf = textureLod(environmentBrdf, vec2(nDotV, roughness), 0.0).rg;
    // A horizon fade removes reflections that a normal map bends below the
    // geometric surface.
    float horizon = min(1.0 + dot(reflected, geometricNormal), 1.0);
    vec3 specular = prefilteredRadiance(reflected, roughness) * (reflectance * brdf.x + brdf.y) *
                    camera.environmentGround.w * specularOcclusion(nDotV, occlusion, roughness) * horizon * horizon;
    return diffuseWeight * diffuse * occlusion + specular;
}

void main() {
    MaterialRecord material = materials.values[materialIndex];
    vec4 sampledBase = sampleMaterialTexture(material.textures.x, texCoord);
    float alpha = sampledBase.a * material.baseColor.a;
    if (material.surface.y > 0.5 && material.surface.y < 1.5 && alpha < material.surface.x) {
        discard;
    }
    vec3 albedo = sampledBase.rgb * material.baseColor.rgb;
    vec4 orm = sampleMaterialTexture(material.textures.z, texCoord);
    float metallic = clamp(orm.b * material.factors.x, 0.0, 1.0);
    float roughness = clamp(orm.g * material.factors.y, 0.045, 1.0);
    float occlusion = mix(1.0, sampleMaterialTexture(material.textures.w, texCoord).r, material.factors.w);
    // Screen-space occlusion describes the opaque depth; blended surfaces in
    // front of it keep only their material occlusion.
    if (material.surface.y < 1.5) {
        occlusion = min(occlusion, screenSpaceAmbientOcclusion());
    }

    vec3 tangent = normalize(worldTangent.xyz);
    vec3 geometricNormal = normalize(worldNormal);
    vec3 normal = geometricNormal;
    tangent = normalize(tangent - normal * dot(normal, tangent));
    vec3 bitangent = cross(normal, tangent) * worldTangent.w;
    // Normal maps may be two-channel (BC5), so Z is rebuilt from unit length.
    vec2 mappedNormalXY = sampleMaterialTexture(material.textures.y, texCoord).xy * 2.0 - 1.0;
    vec3 mappedNormal = vec3(mappedNormalXY, sqrt(clamp(1.0 - dot(mappedNormalXY, mappedNormalXY), 0.0, 1.0)));
    mappedNormal.xy *= material.factors.z;
    normal = normalize(mat3(tangent, bitangent, normal) * mappedNormal);

    vec3 viewDirection = normalize(camera.cameraPosition.xyz - worldPosition);
    vec3 lightDirection = normalize(camera.lightDirection.xyz);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    vec3 reflectance = mix(vec3(0.04), albedo, metallic);
    // The view half of the Smith geometry term is shared by every light.
    float viewGeometry = geometrySchlickGGX(nDotV, roughness);
    float shadow = shadowVisibility(worldPosition, geometricNormal, lightDirection);
    vec3 sunRadiance = camera.lightColor.rgb * camera.lightDirection.w;
    vec3 direct = directResponse(normal, viewDirection, lightDirection, nDotV, nDotL, albedo, metallic, roughness,
                                 reflectance, viewGeometry) *
                  sunRadiance * nDotL * shadow;
    uvec2 clusterLights = camera.clusterGrid.w > 0u ? clusterRanges.values[fragmentCluster()] : uvec2(0u);
    for (uint entry = 0; entry < clusterLights.y; ++entry) {
        LocalLight light = localLights.values[clusterLightIndices.values[clusterLights.x + entry]];
        vec3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        float range = light.positionRange.w;
        if (distanceToLight >= range || distanceToLight <= 0.0001) {
            continue;
        }
        vec3 localDirection = toLight / distanceToLight;
        float attenuation = pow(max(1.0 - distanceToLight / range, 0.0), 2.0) /
                            max(distanceToLight * distanceToLight, 0.01);
        if (light.directionType.w > 0.5) {
            float coneCosine = dot(-localDirection, normalize(light.directionType.xyz));
            attenuation *= smoothstep(light.spotCone.y, light.spotCone.x, coneCosine);
        }
        float localNDotL = max(dot(normal, localDirection), 0.0);
        vec3 localRadiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
        direct += directResponse(normal, viewDirection, localDirection, nDotV, localNDotL, albedo, metallic,
                                 roughness, reflectance, viewGeometry) *
                  localRadiance * localNDotL;
    }
    vec3 indirect;
    if (camera.environmentParams.x > 0.5) {
        indirect = imageBasedLighting(worldPosition, normal, geometricNormal, viewDirection, nDotV, albedo, metallic,
                                      roughness, reflectance, occlusion);
    } else {
        indirect = hemisphereLighting(worldPosition, normal, viewDirection, nDotV, albedo, metallic, roughness,
                                      reflectance, occlusion);
    }
    vec3 emissive = sampleMaterialTexture(material.extraTextures.x, texCoord).rgb * material.emissive.rgb;
    vec3 color = direct + indirect + emissive;
    if (SHADOW_CASCADE_COUNT > 1 && camera.renderingSettings.w > 0.5) {
        const vec3 cascadeColors[3] = vec3[3](vec3(1.0, 0.35, 0.35), vec3(0.35, 1.0, 0.35),
                                              vec3(0.35, 0.55, 1.0));
        color = mix(color, color * cascadeColors[shadowCascade(worldPosition)], 0.45);
    }
    outColor = vec4(color, alpha);
    // Jitter moved each projected point by -jitter in NDC; adding it back gives
    // unjittered positions, so a static scene has zero motion (mirrored by
    // unjitteredMotionUv in TemporalQuality.cpp).
    vec2 currentUv =
        (currentClipPosition.xy / max(abs(currentClipPosition.w), 0.00001) + camera.temporalJitter.xy) * 0.5 + 0.5;
    vec2 previousUv =
        (previousClipPosition.xy / max(abs(previousClipPosition.w), 0.00001) + camera.temporalJitter.zw) * 0.5 + 0.5;
    float reactive = max(material.surface.y > 1.5 ? alpha : 0.0,
                         smoothstep(1.0, 4.0, dot(emissive, vec3(0.2126, 0.7152, 0.0722))));
    // Surfaces revealed by camera motion are detected in taa_resolve.comp against
    // the previous frame's depth (stored in .w); only off-screen history is known here.
    bool previousOffscreen = previousClipPosition.w <= 0.0 || any(lessThan(previousUv, vec2(0.0))) ||
                             any(greaterThan(previousUv, vec2(1.0)));
    outMotionReactive = vec4(currentUv - previousUv, previousOffscreen ? 1.0 : reactive, gl_FragCoord.z);
}
