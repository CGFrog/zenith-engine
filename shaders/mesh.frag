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
    // Reflection probes (U9): x: 1 when per-pixel probe blending is active,
    // y: resident probe count.
    vec4 reflectionProbeParams;
    // Volumetric fog (U10); see VolumetricFog.hpp. x: 1 composites the
    // integrated fog volume; y, z: froxel slice = log(view depth) * y + z;
    // w: froxel depth slices.
    vec4 fogVolume;
    // xy: integrated-volume uv per render pixel (1 / internal render extent).
    vec4 fogScreen;
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
// slot 1 the dominant reflection probe at the camera, mixed by
// camera.environmentParams.y (zero when probes blend per pixel instead).
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
// Integrated volumetric fog (VolumetricFogPass): rgb in-scattered light, a
// transmittance from the eye to the end of each froxel slice.
layout(set = 0, binding = 18) uniform sampler3D volumetricFog;
// Opaque scene behind transmissive surfaces (TransmissionBackground): render
// pixel p maps to uv p * camera.fogScreen.xy; mips are box-filtered.
layout(set = 0, binding = 24) uniform sampler2D transmissionBackground;

// Mirrors MaterialRecord in engine/graphics/MaterialRecord.hpp (std430).
struct MaterialRecord {
    vec4 baseColor;
    vec4 factors;  // metallic, roughness, normal scale, occlusion strength
    vec4 emissive; // rgb radiance; w unused
    vec4 surface;  // alpha cutoff, alpha mode (0 opaque, 1 mask, 2 blend), ior, transmission
    uvec4 textures; // bindless slots: base color, normal, metallic-roughness, occlusion
    uvec4 extraTextures; // emissive slot, lobe flags (LOBE_*), transmission slot, unused
    vec4 clearcoat; // factor, perceptual roughness, normal scale, unused
    vec4 sheen;     // linear rgb color, perceptual roughness
    vec4 specular;  // linear rgb color, factor
    vec4 volume;    // attenuation coefficient per world unit (rgb), thickness
    uvec4 lobeTextures;  // bindless slots: clearcoat, clearcoat roughness, clearcoat normal, sheen color
    uvec4 layerTextures; // bindless slots: sheen roughness, specular, specular color; w unused
};

// Mirror the material_lobes bits in MaterialRecord.hpp.
const uint LOBE_CLEARCOAT = 1u;
const uint LOBE_SHEEN = 2u;
const uint LOBE_TRANSMISSION = 4u;
const uint LOBE_SPECULAR_TEXTURES = 8u;
const uint LOBE_VOLUME = 16u;

layout(std430, set = 0, binding = 4) readonly buffer Materials {
    MaterialRecord values[];
} materials;

// Mirrors GpuLocalLight in ClusteredLightingPass.hpp (std430).
struct LocalLight {
    vec4 positionRange;  // world position, range
    vec4 colorIntensity; // linear color, intensity
    vec4 directionType;  // world direction, type (0 point, 1 spot)
    vec4 spotCone;       // inner and outer cone cosines, first local shadow tile (-1: unshadowed)
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

// Local-light shadow atlas (LocalShadowPass): conventional depth, compared
// LESS_OR_EQUAL. A shadowed light's spotCone.z names its first tile record;
// point lights add the cube face (localShadowFace).
layout(set = 0, binding = 14) uniform sampler2DShadow localShadowAtlas;

// Mirrors GpuLocalShadowTile in LocalShadowPass.hpp (std430).
struct LocalShadowTile {
    mat4 viewProjection; // world to the tile's clip space
    vec4 atlasRect;      // uv of the tile's top-left corner (xy) and its uv extent (zw)
    vec4 params;         // x: shadow-texel world size per unit distance from the light
};

layout(std430, set = 0, binding = 15) readonly buffer LocalShadowTiles {
    LocalShadowTile values[];
} localShadowTiles;

// Per-pixel reflection probes. EnvironmentPass keeps each resident probe's
// prefiltered specular cube in one array; ReflectionProbeClusterPass writes the
// probe records, their irradiance and the per-cluster probe masks.
layout(set = 0, binding = 16) uniform samplerCubeArray reflectionProbeSpecular;

// Mirror maximumResidentReflectionProbes and maximumBlendedReflectionProbes in
// ReflectionProbes.hpp.
const uint MAXIMUM_RESIDENT_REFLECTION_PROBES = 8;
const uint MAXIMUM_BLENDED_REFLECTION_PROBES = 4;

// Mirrors GpuReflectionProbe in ReflectionProbeClusterPass.hpp (std430).
struct ReflectionProbeRecord {
    vec4 centerBlend; // world box center, blend distance
    vec4 halfExtents; // world box half extents
    ivec4 parameters; // x: priority, y: probe cache slot
    vec4 viewMinimum; // view-space influence bounds, used by the cluster pass
    vec4 viewMaximum;
};

layout(std430, set = 0, binding = 17) readonly buffer ReflectionProbes {
    uvec4 header; // x: probe count
    ReflectionProbeRecord records[MAXIMUM_RESIDENT_REFLECTION_PROBES];
    // Nine irradiance coefficients per record, as in EnvironmentIrradiance.
    vec4 irradiance[MAXIMUM_RESIDENT_REFLECTION_PROBES * 9];
    // Per cluster: bit i set when record i may influence the cluster.
    uint clusterMasks[];
} reflectionProbes;

// Clustered decals (DecalClusterPass). Mirror maximumDecals,
// maximumDecalsPerCluster and decalDepthFadeFraction in Decals.hpp and the
// texture flags in DecalClusterPass.hpp.
const uint MAXIMUM_DECALS = 1024;
const uint MAXIMUM_DECALS_PER_CLUSTER = 31;
const uint DECAL_CLUSTER_STRIDE = MAXIMUM_DECALS_PER_CLUSTER + 1;
const uint DECAL_HAS_ALBEDO = 1u;
const uint DECAL_HAS_NORMAL = 2u;
const float DECAL_DEPTH_FADE_FRACTION = 0.25;

// Mirrors GpuDecal in DecalClusterPass.hpp (std430).
struct DecalRecord {
    vec4 worldToDecal[3]; // rows of the world-to-decal-space transform
    vec4 tintOpacity;     // linear tint, opacity
    vec4 surface;         // roughness, roughness strength, metallic, metallic strength
    vec4 blend;           // base color strength, normal strength, angle fade scale, angle fade bias
    uvec4 textures;       // albedo slot (sRGB), normal slot (linear), DECAL_HAS_* flags
    vec4 viewMinimum;     // view-space bounds, used by the cluster pass
    vec4 viewMaximum;
};

layout(std430, set = 0, binding = 19) readonly buffer Decals {
    uvec4 header; // x: decal count; zero when decals are off or absent
    DecalRecord records[MAXIMUM_DECALS];
} decals;

// Written by decal_clusters.comp: per cluster a count, then record indices in
// application order.
layout(std430, set = 0, binding = 20) readonly buffer DecalClusterLists {
    uint values[];
} decalClusterLists;

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

// Cube face of a point light's shadow: +X, -X, +Y, -Y, +Z, -Z by the major
// axis of lightToPoint. Mirrors localShadowFace in LocalShadows.cpp.
uint localShadowFace(vec3 lightToPoint) {
    vec3 magnitude = abs(lightToPoint);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
        return lightToPoint.x >= 0.0 ? 0u : 1u;
    }
    if (magnitude.y >= magnitude.z) {
        return lightToPoint.y >= 0.0 ? 2u : 3u;
    }
    return lightToPoint.z >= 0.0 ? 4u : 5u;
}

// Fraction of a local light reaching the surface (1 when the light has no
// shadow tile). The receiver moves along its geometric normal by about one
// shadow texel, more as the surface turns away from the light (slope-scaled),
// and four bilinear comparisons filter a 3x3-texel footprint clamped inside
// the tile so neighbouring tiles never bleed in.
float localShadowVisibility(LocalLight light, vec3 position, vec3 geometricNormal, vec3 toLight,
                            float distanceToLight) {
    if (light.spotCone.z < 0.0) {
        return 1.0;
    }
    uint tileIndex = uint(light.spotCone.z + 0.5);
    if (light.directionType.w < 0.5) {
        tileIndex += localShadowFace(position - light.positionRange.xyz);
    }
    LocalShadowTile tile = localShadowTiles.values[tileIndex];
    float cosine = clamp(dot(geometricNormal, toLight), 0.0, 1.0);
    float slope = min(sqrt(1.0 - cosine * cosine) / max(cosine, 0.05), 4.0);
    float texelWorldSize = tile.params.x * distanceToLight;
    vec4 clip = tile.viewProjection * vec4(position + geometricNormal * texelWorldSize * (1.0 + slope), 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 projected = clip.xyz / clip.w;
    // Beyond the light's range, or outside a tile that cannot cover a very
    // wide spot cone (see localShadowTanHalfFov): treat as unshadowed rather
    // than comparing against unrelated border texels.
    if (projected.z >= 1.0 || any(greaterThan(abs(projected.xy), vec2(1.0)))) {
        return 1.0;
    }
    vec2 atlasTexel = 1.0 / vec2(textureSize(localShadowAtlas, 0));
    vec2 tileMinimum = tile.atlasRect.xy + atlasTexel * 1.5;
    vec2 tileMaximum = tile.atlasRect.xy + tile.atlasRect.zw - atlasTexel * 1.5;
    vec2 center = tile.atlasRect.xy + (projected.xy * 0.5 + 0.5) * tile.atlasRect.zw;
    float visibility = 0.0;
    for (int tap = 0; tap < 4; ++tap) {
        vec2 offset = (vec2(tap & 1, tap >> 1) - 0.5) * atlasTexel;
        visibility += texture(localShadowAtlas, vec3(clamp(center + offset, tileMinimum, tileMaximum), projected.z));
    }
    return visibility * 0.25;
}

// Reflected fraction of one light's irradiance, excluding radiance and the cosine
// term. The fast variant is unnormalized Lambert, matching its tuned brightness.
// specularWeight scales the Fresnel term, grazing value included
// (KHR_materials_specular); 1 is the plain metallic-roughness model.
vec3 directResponse(vec3 normal, vec3 viewDirection, vec3 lightDirection, float nDotV, float nDotL, vec3 albedo,
                    float metallic, float roughness, vec3 reflectance, float specularWeight, float viewGeometry) {
    if (!PHYSICALLY_BASED_LIGHTING) {
        return albedo;
    }
    vec3 halfVector = normalize(viewDirection + lightDirection);
    vec3 fresnel = specularWeight * fresnelSchlick(max(dot(halfVector, viewDirection), 0.0), reflectance);
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

// Composites the fog between the eye and this fragment; mirrors
// FroxelGrid::integratedW for the depth lookup.
vec3 applyVolumetricFog(vec3 color) {
    if (camera.fogVolume.x < 0.5) {
        return color;
    }
    float viewDepth = camera.clusterDepth.z / (gl_FragCoord.z + camera.clusterDepth.w);
    float slices = camera.fogVolume.w;
    float slice = clamp(log(viewDepth) * camera.fogVolume.y + camera.fogVolume.z, 0.0, slices);
    float w = clamp((slice - 0.5) / slices, 0.5 / slices, (slices - 0.5) / slices);
    vec4 fog = textureLod(volumetricFog, vec3(gl_FragCoord.xy * camera.fogScreen.xy, w), 0.0);
    return color * fog.a + fog.rgb;
}

// Flat sky/ground approximation used when image-based lighting is disabled.
// Occlusion applies to the diffuse term and, as specular occlusion, to the
// reflection term.
vec3 hemisphereLighting(vec3 position, vec3 normal, vec3 viewDirection, float nDotV, vec3 albedo, float metallic,
                        float roughness, vec3 reflectance, float specularWeight, float occlusion) {
    float hemisphereWeight = normal.y * 0.5 + 0.5;
    vec3 diffuseEnvironment = mix(camera.environmentGround.rgb, camera.environmentSky.rgb, hemisphereWeight);
    vec3 indirect = albedo * indirectDiffuseRadiance(diffuseEnvironment * camera.environmentSky.w, position, normal,
                                                     viewDirection);
    if (!PHYSICALLY_BASED_LIGHTING) {
        return indirect * occlusion;
    }
    vec3 indirectFresnel = specularWeight * fresnelSchlickRoughness(nDotV, reflectance, roughness);
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
                        vec3 albedo, float metallic, float roughness, vec3 reflectance, float specularWeight,
                        float occlusion) {
    ProbeBlend probeBlend = blendReflectionProbes(position);
    // The irradiance volume replaces the probe-blended environment term inside
    // its box and fades back to it across the box edge.
    vec3 diffuse = albedo * indirectDiffuseRadiance(environmentIrradianceAt(normal, probeBlend) *
                                                        camera.environmentSky.w,
                                                    position, normal, viewDirection);
    if (!PHYSICALLY_BASED_LIGHTING) {
        return diffuse * occlusion;
    }
    vec3 diffuseWeight =
        (vec3(1.0) - specularWeight * fresnelSchlickRoughness(nDotV, reflectance, roughness)) * (1.0 - metallic);
    vec3 reflected = reflect(-viewDirection, normal);
    vec2 brdf = textureLod(environmentBrdf, vec2(nDotV, roughness), 0.0).rg;
    // A horizon fade removes reflections that a normal map bends below the
    // geometric surface.
    float horizon = min(1.0 + dot(reflected, geometricNormal), 1.0);
    vec3 specular = prefilteredRadiance(worldPosition, reflected, roughness, probeBlend) *
                    (reflectance * brdf.x + brdf.y) * specularWeight * camera.environmentGround.w * specularOcclusion(nDotV, occlusion, roughness) * horizon * horizon;
    return diffuseWeight * diffuse * occlusion + specular;
}

vec4 sampleDecalTexture(uint slot, vec2 uv, vec2 uvDx, vec2 uvDy) {
    return textureGrad(sampler2D(materialTextures[nonuniformEXT(slot)], materialSampler), uv, uvDx, uvDy);
}

// Blends this fragment's cluster decals into the surface before lighting, in
// application order, each moving base color, normal, roughness and metallic
// toward its own by its mask (see decalCoverage in Decals.cpp). Decal normals
// are tangent-space maps reoriented onto the current normal along the decal's
// u axis; decals without one flatten the normal toward the geometric normal.
// Must be called in uniform control flow: decal textures filter with
// gradients derived from the world-position derivatives taken here.
void applyDecals(vec3 position, vec3 geometricNormal, inout vec3 albedo, inout vec3 normal, inout float roughness,
                 inout float metallic) {
    vec3 positionDx = dFdx(position);
    vec3 positionDy = dFdy(position);
    if (decals.header.x == 0u) {
        return;
    }
    uint listStart = fragmentCluster() * DECAL_CLUSTER_STRIDE;
    uint listed = min(decalClusterLists.values[listStart], MAXIMUM_DECALS_PER_CLUSTER);
    vec4 worldPosition = vec4(position, 1.0);
    for (uint entry = 0; entry < listed; ++entry) {
        DecalRecord decal = decals.records[decalClusterLists.values[listStart + 1u + entry]];
        vec3 local = vec3(dot(decal.worldToDecal[0], worldPosition), dot(decal.worldToDecal[1], worldPosition),
                          dot(decal.worldToDecal[2], worldPosition));
        if (any(greaterThan(abs(local), vec3(1.0)))) {
            continue;
        }
        vec3 facing = normalize(decal.worldToDecal[1].xyz);
        float angleFade = clamp(dot(geometricNormal, facing) * decal.blend.z + decal.blend.w, 0.0, 1.0);
        float depthFade = clamp((1.0 - abs(local.y)) / DECAL_DEPTH_FADE_FRACTION, 0.0, 1.0);
        float mask = decal.tintOpacity.a * angleFade * depthFade;
        if (mask <= 0.0) {
            continue;
        }
        vec2 uv = local.xz * 0.5 + 0.5;
        vec2 uvDx = vec2(dot(decal.worldToDecal[0].xyz, positionDx), dot(decal.worldToDecal[2].xyz, positionDx)) * 0.5;
        vec2 uvDy = vec2(dot(decal.worldToDecal[0].xyz, positionDy), dot(decal.worldToDecal[2].xyz, positionDy)) * 0.5;
        vec3 decalColor = decal.tintOpacity.rgb;
        if ((decal.textures.z & DECAL_HAS_ALBEDO) != 0u) {
            vec4 sampledAlbedo = sampleDecalTexture(decal.textures.x, uv, uvDx, uvDy);
            decalColor *= sampledAlbedo.rgb;
            mask *= sampledAlbedo.a;
        }
        albedo = mix(albedo, decalColor, mask * decal.blend.x);
        if ((decal.textures.z & DECAL_HAS_NORMAL) != 0u) {
            vec3 tangent = decal.worldToDecal[0].xyz - normal * dot(normal, decal.worldToDecal[0].xyz);
            float tangentLength = length(tangent);
            if (tangentLength > 1.0e-6) {
                tangent /= tangentLength;
                // Texture +Y points toward decreasing v, which is decal-space -Z.
                vec3 bitangent = cross(normal, tangent);
                bitangent *= dot(bitangent, decal.worldToDecal[2].xyz) > 0.0 ? -1.0 : 1.0;
                vec2 mappedXY = sampleDecalTexture(decal.textures.y, uv, uvDx, uvDy).xy * 2.0 - 1.0;
                vec3 mapped = vec3(mappedXY, sqrt(clamp(1.0 - dot(mappedXY, mappedXY), 0.0, 1.0)));
                vec3 decalNormal = normalize(mat3(tangent, bitangent, normal) * mapped);
                normal = normalize(mix(normal, decalNormal, mask * decal.blend.y));
            }
        } else {
            // Without a normal map the decal fills the surface's bumps, as
            // paint or standing water does.
            normal = normalize(mix(normal, geometricNormal, mask * decal.blend.y));
        }
        roughness = mix(roughness, decal.surface.x, mask * decal.surface.y);
        metallic = mix(metallic, decal.surface.z, mask * decal.surface.w);
    }
    roughness = clamp(roughness, 0.045, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// KHR material extensions layered over metallic-roughness, following the glTF
// specifications and the Khronos sample viewer:
//   ior, specular: dielectric F0 and specular weight (dielectricSpecular).
//   transmission (+ volume): the diffuse base is replaced by the opaque scene
//     behind the surface (transmittedRadiance).
//   sheen: Charlie lobe over the base, which is scaled by 1 - max(sheen color)
//     * sheen albedo (environmentBrdf.b).
//   clearcoat: GGX lobe of a 1.5-ior coat over everything below it, which is
//     attenuated by the coat's Fresnel at the view angle.
// Each lobe runs only when its LOBE_* bit is set, a branch uniform per
// material.
// ---------------------------------------------------------------------------

// Per-fragment inputs of the optional lobes; values of disabled lobes are
// neutral (zero coat, black sheen, no transmission).
struct SurfaceLobes {
    uint flags;
    float clearcoat;
    float clearcoatRoughness;
    vec3 clearcoatNormal;
    vec3 sheenColor;
    float sheenRoughness;
    // Energy the sheen takes from the layers below it.
    float sheenAlbedoScaling;
    // Fraction of the light below the coat that leaves through it.
    float clearcoatAttenuation;
    float transmission;
};

const uint LOBE_LAYERS = LOBE_SHEEN | LOBE_CLEARCOAT;

// Mirror the functions of the same names in env_brdf_lut.comp and
// EnvironmentLighting.cpp.
float charlieDistribution(float nDotH, float alpha) {
    float inverseAlpha = 1.0 / alpha;
    float sinSquared = max(1.0 - nDotH * nDotH, 0.0);
    return (2.0 + inverseAlpha) * pow(sinSquared, 0.5 * inverseAlpha) / (2.0 * PI);
}

float sheenLambdaHelper(float x, float alpha) {
    float oneMinusAlphaSquared = (1.0 - alpha) * (1.0 - alpha);
    float a = mix(21.5473, 25.3245, oneMinusAlphaSquared);
    float b = mix(3.82987, 3.32435, oneMinusAlphaSquared);
    float c = mix(0.19823, 0.16801, oneMinusAlphaSquared);
    float d = mix(-1.97760, -1.27393, oneMinusAlphaSquared);
    float e = mix(-4.32054, -4.85967, oneMinusAlphaSquared);
    return a / (1.0 + b * pow(x, c)) + d * x + e;
}

float sheenLambda(float cosine, float alpha) {
    if (abs(cosine) < 0.5) {
        return exp(sheenLambdaHelper(cosine, alpha));
    }
    return exp(2.0 * sheenLambdaHelper(0.5, alpha) - sheenLambdaHelper(1.0 - cosine, alpha));
}

float sheenVisibility(float nDotL, float nDotV, float alpha) {
    float denominator = (1.0 + sheenLambda(nDotV, alpha) + sheenLambda(nDotL, alpha)) * (4.0 * nDotV * nDotL);
    return clamp(1.0 / max(denominator, 1e-6), 0.0, 1.0);
}

float sheenAlpha(float sheenRoughness) {
    return max(sheenRoughness * sheenRoughness, 1e-6);
}

// KHR_materials_ior and KHR_materials_specular: dielectric reflectance at
// normal incidence (rgb) and the specular weight (a), which scales the
// dielectric Fresnel term including its grazing value. ior 1.5 with the
// default specular gives (0.04, 1), the metallic-roughness model.
vec4 dielectricSpecular(MaterialRecord material, vec2 uv) {
    float ior = material.surface.z;
    float reflectance = (ior - 1.0) / (ior + 1.0);
    vec3 color = material.specular.rgb;
    float weight = material.specular.w;
    if ((material.extraTextures.y & LOBE_SPECULAR_TEXTURES) != 0u) {
        weight *= sampleMaterialTexture(material.layerTextures.y, uv).a;
        color *= sampleMaterialTexture(material.layerTextures.z, uv).rgb;
    }
    return vec4(min(vec3(reflectance * reflectance) * color, vec3(1.0)), weight);
}

// tangentFrame maps tangent space to world space around the geometric normal.
SurfaceLobes surfaceLobes(MaterialRecord material, vec2 uv, mat3 tangentFrame, vec3 viewDirection, float nDotV) {
    SurfaceLobes lobes;
    // The fast lighting variant keeps only transmission, so glass stays
    // see-through; sheen and the coat need the physically based model.
    const uint fastLobes = LOBE_TRANSMISSION | LOBE_VOLUME;
    lobes.flags = material.extraTextures.y & (PHYSICALLY_BASED_LIGHTING ? 0xffffffffu : fastLobes);
    lobes.clearcoat = 0.0;
    lobes.clearcoatRoughness = 1.0;
    lobes.clearcoatNormal = tangentFrame[2];
    lobes.sheenColor = vec3(0.0);
    lobes.sheenRoughness = 0.0;
    lobes.sheenAlbedoScaling = 1.0;
    lobes.clearcoatAttenuation = 1.0;
    lobes.transmission = 0.0;
    if ((lobes.flags & LOBE_CLEARCOAT) != 0u) {
        lobes.clearcoat = material.clearcoat.x * sampleMaterialTexture(material.lobeTextures.x, uv).r;
        lobes.clearcoatRoughness =
            clamp(material.clearcoat.y * sampleMaterialTexture(material.lobeTextures.y, uv).g, 0.045, 1.0);
        // Without a coat normal map the flat fallback leaves the geometric
        // normal, as the extension specifies.
        vec2 mappedXY = sampleMaterialTexture(material.lobeTextures.z, uv).xy * 2.0 - 1.0;
        vec3 mapped = vec3(mappedXY * material.clearcoat.z, sqrt(clamp(1.0 - dot(mappedXY, mappedXY), 0.0, 1.0)));
        lobes.clearcoatNormal = normalize(tangentFrame * mapped);
        float coatNDotV = max(dot(lobes.clearcoatNormal, viewDirection), 0.0);
        lobes.clearcoatAttenuation = 1.0 - lobes.clearcoat * fresnelSchlick(coatNDotV, vec3(0.04)).x;
    }
    if ((lobes.flags & LOBE_SHEEN) != 0u) {
        lobes.sheenColor = material.sheen.rgb * sampleMaterialTexture(material.lobeTextures.w, uv).rgb;
        lobes.sheenRoughness = material.sheen.w * sampleMaterialTexture(material.layerTextures.x, uv).a;
        float albedo = textureLod(environmentBrdf, vec2(nDotV, lobes.sheenRoughness), 0.0).b;
        float strongest = max(lobes.sheenColor.r, max(lobes.sheenColor.g, lobes.sheenColor.b));
        lobes.sheenAlbedoScaling = 1.0 - strongest * albedo;
    }
    if ((lobes.flags & LOBE_TRANSMISSION) != 0u) {
        lobes.transmission = material.surface.w * sampleMaterialTexture(material.extraTextures.z, uv).r;
    }
    return lobes;
}

// Sheen and clearcoat response to one light, each weighted by its own cosine;
// the sheen is already attenuated by the coat above it. main() scales the
// base layer by sheenAlbedoScaling and clearcoatAttenuation.
vec3 lobeDirectResponse(SurfaceLobes lobes, vec3 normal, vec3 viewDirection, vec3 lightDirection, float nDotV,
                        float nDotL) {
    if ((lobes.flags & LOBE_LAYERS) == 0u) {
        return vec3(0.0);
    }
    vec3 response = vec3(0.0);
    vec3 halfVector = normalize(viewDirection + lightDirection);
    if ((lobes.flags & LOBE_SHEEN) != 0u && nDotL > 0.0) {
        float alpha = sheenAlpha(lobes.sheenRoughness);
        response += lobes.sheenColor * charlieDistribution(max(dot(normal, halfVector), 0.0), alpha) *
                    sheenVisibility(nDotL, max(nDotV, 1e-4), alpha) * nDotL * lobes.clearcoatAttenuation;
    }
    if ((lobes.flags & LOBE_CLEARCOAT) != 0u) {
        vec3 coatNormal = lobes.clearcoatNormal;
        float coatNDotL = max(dot(coatNormal, lightDirection), 0.0);
        if (coatNDotL > 0.0) {
            float coatNDotV = max(dot(coatNormal, viewDirection), 1e-4);
            float roughness = lobes.clearcoatRoughness;
            float fresnel = fresnelSchlick(max(dot(halfVector, viewDirection), 0.0), vec3(0.04)).x;
            float geometry = geometrySchlickGGX(coatNDotV, roughness) * geometrySchlickGGX(coatNDotL, roughness);
            response += lobes.clearcoat * fresnel * distributionGGX(coatNormal, halfVector, roughness) * geometry /
                        max(4.0 * coatNDotV, 0.0001);
        }
    }
    return response;
}

// Ambient light of the sheen (attenuated by the coat) and clearcoat lobes from
// the same sources as the base layer: image-based lighting or the hemisphere
// fallback.
vec3 lobeIndirectResponse(SurfaceLobes lobes, vec3 position, vec3 normal, vec3 viewDirection, float nDotV,
                          float occlusion) {
    if ((lobes.flags & LOBE_LAYERS) == 0u) {
        return vec3(0.0);
    }
    bool imageBased = camera.environmentParams.x > 0.5;
    ProbeBlend probeBlend;
    if (imageBased) {
        probeBlend = blendReflectionProbes(position);
    }
    vec3 response = vec3(0.0);
    if ((lobes.flags & LOBE_SHEEN) != 0u) {
        // The broad sheen lobe is lit by the cosine-weighted environment, the
        // radiance a white diffuse surface reflects, scaled by its albedo.
        vec3 environment = imageBased ? environmentIrradianceAt(normal, probeBlend)
                                      : mix(camera.environmentGround.rgb, camera.environmentSky.rgb,
                                            normal.y * 0.5 + 0.5);
        vec3 white = indirectDiffuseRadiance(environment * camera.environmentSky.w, position, normal, viewDirection);
        float albedo = textureLod(environmentBrdf, vec2(nDotV, lobes.sheenRoughness), 0.0).b;
        response += lobes.sheenColor * albedo * white * occlusion * lobes.clearcoatAttenuation;
    }
    if ((lobes.flags & LOBE_CLEARCOAT) != 0u) {
        vec3 coatNormal = lobes.clearcoatNormal;
        float coatNDotV = max(dot(coatNormal, viewDirection), 1e-4);
        float roughness = lobes.clearcoatRoughness;
        vec3 reflected = reflect(-viewDirection, coatNormal);
        vec3 radiance;
        float reflectance;
        if (imageBased) {
            radiance = prefilteredRadiance(position, reflected, roughness, probeBlend);
            vec2 brdf = textureLod(environmentBrdf, vec2(coatNDotV, roughness), 0.0).rg;
            reflectance = 0.04 * brdf.x + brdf.y;
        } else {
            float level = roughness * 5.0;
            int low = int(floor(level));
            radiance = mix(camera.environmentSpecular[low].rgb, camera.environmentSpecular[min(low + 1, 5)].rgb,
                           fract(level)) *
                       mix(1.0, 0.35, roughness);
            reflectance = fresnelSchlickRoughness(coatNDotV, vec3(0.04), roughness).x;
        }
        response += lobes.clearcoat * radiance * reflectance * camera.environmentGround.w *
                    specularOcclusion(coatNDotV, occlusion, roughness);
    }
    return response;
}

// KHR_materials_transmission: light from the opaque scene behind the surface,
// tinted by the base color and reduced by what the surface reflects. Thin
// walled (straight through) unless KHR_materials_volume gives a thickness,
// which refracts the view ray by the ior and absorbs along it. Roughness blurs
// through the background's mips. Light sources behind the surface reach it
// only through the background, not as direct transmission.
vec3 transmittedRadiance(MaterialRecord material, SurfaceLobes lobes, vec3 normal, vec3 viewDirection, float nDotV,
                         vec3 albedo, float metallic, float roughness, vec3 reflectance, float specularWeight) {
    float ior = material.surface.z;
    vec2 uv = gl_FragCoord.xy * camera.fogScreen.xy;
    vec3 absorption = vec3(1.0);
    if ((lobes.flags & LOBE_VOLUME) != 0u) {
        // Thickness is in world units (object scale is not applied).
        float thickness = material.volume.w;
        vec3 refracted = refract(-viewDirection, normal, 1.0 / ior);
        vec4 exitClip = camera.viewProjection * vec4(worldPosition + refracted * thickness, 1.0);
        if (exitClip.w > 0.0) {
            uv = exitClip.xy / exitClip.w * 0.5 + 0.5;
        }
        absorption = exp(-material.volume.rgb * thickness);
    }
    float lod = log2(float(textureSize(transmissionBackground, 0).x)) * roughness * clamp(ior * 2.0 - 2.0, 0.0, 1.0);
    vec3 background = textureLod(transmissionBackground, uv, lod).rgb;
    vec3 reflected = vec3(0.0);
    if (PHYSICALLY_BASED_LIGHTING) {
        vec2 brdf = textureLod(environmentBrdf, vec2(nDotV, roughness), 0.0).rg;
        reflected = specularWeight * (reflectance * brdf.x + brdf.y);
    }
    return lobes.transmission * (1.0 - metallic) * (vec3(1.0) - reflected) * albedo * absorption * background;
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
    bool transmissive = (material.extraTextures.y & LOBE_TRANSMISSION) != 0u;
    if (material.surface.y < 1.5 && !transmissive) {
        occlusion = min(occlusion, screenSpaceAmbientOcclusion());
    }
    // Transmissive surfaces show the scene behind through their color, not
    // their coverage (unless blended as well).
    if (transmissive && material.surface.y < 1.5) {
        alpha = 1.0;
    }

    vec3 tangent = normalize(worldTangent.xyz);
    vec3 geometricNormal = normalize(worldNormal);
    vec3 normal = geometricNormal;
    tangent = normalize(tangent - normal * dot(normal, tangent));
    vec3 bitangent = cross(normal, tangent) * worldTangent.w;
    mat3 tangentFrame = mat3(tangent, bitangent, geometricNormal);
    // Normal maps may be two-channel (BC5), so Z is rebuilt from unit length.
    vec2 mappedNormalXY = sampleMaterialTexture(material.textures.y, texCoord).xy * 2.0 - 1.0;
    vec3 mappedNormal = vec3(mappedNormalXY, sqrt(clamp(1.0 - dot(mappedNormalXY, mappedNormalXY), 0.0, 1.0)));
    mappedNormal.xy *= material.factors.z;
    normal = normalize(tangentFrame * mappedNormal);
    // Decals paint opaque and masked surfaces; blended surfaces keep their own.
    if (material.surface.y < 1.5) {
        applyDecals(worldPosition, geometricNormal, albedo, normal, roughness, metallic);
    }

    vec3 viewDirection = normalize(camera.cameraPosition.xyz - worldPosition);
    vec3 lightDirection = normalize(camera.lightDirection.xyz);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    SurfaceLobes lobes = surfaceLobes(material, texCoord, tangentFrame, viewDirection, nDotV);
    vec4 dielectric = dielectricSpecular(material, texCoord);
    vec3 reflectance = mix(dielectric.rgb, albedo, metallic);
    float specularWeight = mix(dielectric.a, 1.0, metallic);
    // Transmission replaces part of the diffuse base (transmittedRadiance).
    vec3 diffuseAlbedo = albedo * (1.0 - lobes.transmission);
    // The view half of the Smith geometry term is shared by every light.
    float viewGeometry = geometrySchlickGGX(nDotV, roughness);
    float shadow = shadowVisibility(worldPosition, geometricNormal, lightDirection);
    vec3 sunRadiance = camera.lightColor.rgb * camera.lightDirection.w;
    vec3 direct = directResponse(normal, viewDirection, lightDirection, nDotV, nDotL, diffuseAlbedo, metallic,
                                 roughness, reflectance, specularWeight, viewGeometry) *
                  sunRadiance * nDotL * shadow;
    vec3 lobeLight =
        lobeDirectResponse(lobes, normal, viewDirection, lightDirection, nDotV, nDotL) * sunRadiance * shadow;
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
        if (localNDotL <= 0.0 || attenuation <= 0.0) {
            continue;
        }
        attenuation *= localShadowVisibility(light, worldPosition, geometricNormal, localDirection, distanceToLight);
        vec3 localRadiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
        direct += directResponse(normal, viewDirection, localDirection, nDotV, localNDotL, diffuseAlbedo, metallic,
                                 roughness, reflectance, specularWeight, viewGeometry) *
                  localRadiance * localNDotL;
        lobeLight +=
            lobeDirectResponse(lobes, normal, viewDirection, localDirection, nDotV, localNDotL) * localRadiance;
    }
    vec3 indirect;
    if (camera.environmentParams.x > 0.5) {
        indirect = imageBasedLighting(worldPosition, normal, geometricNormal, viewDirection, nDotV, diffuseAlbedo,
                                      metallic, roughness, reflectance, specularWeight, occlusion);
    } else {
        indirect = hemisphereLighting(worldPosition, normal, viewDirection, nDotV, diffuseAlbedo, metallic, roughness,
                                      reflectance, specularWeight, occlusion);
    }
    lobeLight += lobeIndirectResponse(lobes, worldPosition, normal, viewDirection, nDotV, occlusion);
    vec3 emissive = sampleMaterialTexture(material.extraTextures.x, texCoord).rgb * material.emissive.rgb;
    // Base and emission sit below the sheen, which sits below the coat.
    float baseScale = lobes.sheenAlbedoScaling * lobes.clearcoatAttenuation;
    vec3 color = (direct + indirect) * baseScale + emissive * lobes.clearcoatAttenuation + lobeLight;
    // The background already carries the fog in front of it, so transmitted
    // light joins after this surface's own fog.
    vec3 transmitted = vec3(0.0);
    if ((lobes.flags & LOBE_TRANSMISSION) != 0u) {
        transmitted = transmittedRadiance(material, lobes, normal, viewDirection, nDotV, albedo, metallic, roughness,
                                          reflectance, specularWeight) *
                      baseScale;
    }
    if (SHADOW_CASCADE_COUNT > 1 && camera.renderingSettings.w > 0.5) {
        const vec3 cascadeColors[3] = vec3[3](vec3(1.0, 0.35, 0.35), vec3(0.35, 1.0, 0.35),
                                              vec3(0.35, 0.55, 1.0));
        color = mix(color, color * cascadeColors[shadowCascade(worldPosition)], 0.45);
    }
    outColor = vec4(applyVolumetricFog(color) + transmitted, alpha);
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
