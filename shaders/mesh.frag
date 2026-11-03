#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "constants.glsl"

// Quality variants baked in by MeshPass::meshPipelines so disabled paths compile out.
layout(constant_id = 0) const bool PHYSICALLY_BASED_LIGHTING = true;
layout(constant_id = 1) const int SHADOW_CASCADE_COUNT = 3;
// Solid and Wireframe view shading (ViewShading.hpp): one untextured clay
// material lit by the camera constants' studio rig.
layout(constant_id = 2) const bool SOLID_SURFACES = false;

#include "camera.glsl"

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

#include "irradiance_volume.glsl"
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
// The view's depth pyramid (DepthPyramid.hpp): level 0 is the opaque scene
// depth, reversed Z, read with texelFetch at render pixels.
layout(set = 0, binding = 25) uniform sampler2D sceneDepth;

#include "material.glsl"

layout(std430, set = 0, binding = 4) readonly buffer Materials {
    MaterialRecord values[];
} materials;

#include "local_light.glsl"

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

#include "local_shadow_tile.glsl"

layout(std430, set = 0, binding = 15) readonly buffer LocalShadowTiles {
    LocalShadowTile values[];
} localShadowTiles;

// Per-pixel reflection probes. EnvironmentPass keeps each resident probe's
// prefiltered specular cube in one array; ReflectionProbeClusterPass writes the
// probe records, their irradiance and the per-cluster probe masks.
layout(set = 0, binding = 16) uniform samplerCubeArray reflectionProbeSpecular;

#include "reflection_probe.glsl"

layout(std430, set = 0, binding = 17) readonly buffer ReflectionProbes {
    uvec4 header; // x: probe count
    ReflectionProbeRecord records[MAXIMUM_RESIDENT_REFLECTION_PROBES];
    // Nine irradiance coefficients per record, as in EnvironmentIrradiance.
    vec4 irradiance[MAXIMUM_RESIDENT_REFLECTION_PROBES * 9];
    // Per cluster: bit i set when record i may influence the cluster.
    uint clusterMasks[];
} reflectionProbes;

#include "decal.glsl"

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
#include "material_textures.glsl"

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 texCoord;
layout(location = 3) in vec4 worldTangent;
layout(location = 4) in vec4 currentClipPosition;
layout(location = 5) in vec4 previousClipPosition;
layout(location = 6) flat in uint materialIndex;
layout(location = 7) in vec4 surfaceWeights;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outMotionReactive;

// Cluster of the current fragment; mirrors ClusterGrid::clusterIndexAt.
uint fragmentCluster() {
    uvec2 tile = min(uvec2(gl_FragCoord.xy * camera.clusterTileScale.xy), camera.clusterGrid.xy - 1u);
    float viewDepth = camera.clusterDepth.z / (gl_FragCoord.z + camera.clusterDepth.w);
    float slice = log(viewDepth) * camera.clusterDepth.x + camera.clusterDepth.y;
    uint depthSlice = min(uint(max(slice, 0.0)), camera.clusterGrid.z - 1u);
    return tile.x + camera.clusterGrid.x * (tile.y + camera.clusterGrid.y * depthSlice);
}

// The shading, by concern, in dependency order (shaders/mesh/). Each part
// reads the bindings and inputs above.
#include "mesh/terrain.glsl"
#include "mesh/brdf.glsl"
#include "mesh/shadows.glsl"
#include "mesh/local_light_radiance.glsl"
#include "mesh/irradiance.glsl"
#include "mesh/reflection_probes.glsl"
#include "mesh/ambient_lighting.glsl"
#include "mesh/decals.glsl"
#include "mesh/surface_lobes.glsl"
#include "mesh/water.glsl"

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

// Screen-space motion (xy), reactive mask (z) and depth (w) for the temporal
// resolve.
vec4 motionReactive(float reactive) {
    // Jitter moved each projected point by -jitter in NDC; adding it back gives
    // unjittered positions, so a static scene has zero motion (mirrored by
    // unjitteredMotionUv in TemporalQuality.cpp).
    vec2 currentUv =
        (currentClipPosition.xy / max(abs(currentClipPosition.w), 0.00001) + camera.temporalJitter.xy) * 0.5 + 0.5;
    vec2 previousUv =
        (previousClipPosition.xy / max(abs(previousClipPosition.w), 0.00001) + camera.temporalJitter.zw) * 0.5 + 0.5;
    // Surfaces revealed by camera motion are detected in taa_resolve.comp against
    // the previous frame's depth (stored in .w); only off-screen history is known here.
    bool previousOffscreen = previousClipPosition.w <= 0.0 || any(lessThan(previousUv, vec2(0.0))) ||
                             any(greaterThan(previousUv, vec2(1.0)));
    return vec4(currentUv - previousUv, previousOffscreen ? 1.0 : reactive, gl_FragCoord.z);
}

// Solid view shading: a neutral clay albedo under the studio key light and
// hemisphere, plus a faint rim that keeps silhouettes readable against the
// flat background. Normals are geometric; back faces of double-sided surfaces
// face the viewer.
vec3 solidSurfaceColor() {
    const vec3 clay = vec3(0.8);
    const float rimStrength = 0.12;
    vec3 normal = normalize(worldNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    vec3 viewDirection = normalize(camera.cameraPosition.xyz - worldPosition);
    vec3 lightDirection = normalize(camera.lightDirection.xyz);
    vec3 key = camera.lightColor.rgb * camera.lightDirection.w * max(dot(normal, lightDirection), 0.0);
    vec3 ambient = mix(camera.environmentGround.rgb, camera.environmentSky.rgb, normal.y * 0.5 + 0.5) *
                   camera.environmentSky.w;
    float rim = pow(1.0 - max(dot(normal, viewDirection), 0.0), 3.0) * rimStrength;
    return clay * (key / PI + ambient) + rim;
}

void main() {
    MaterialRecord material = materials.values[materialIndex];
    vec4 sampledBase = sampleMaterialTexture(material.textures.x, texCoord);
    float alpha = sampledBase.a * material.baseColor.a;
    if (material.surface.y > 0.5 && material.surface.y < 1.5 && alpha < material.surface.x) {
        discard;
    }
    if (SOLID_SURFACES) {
        // Coverage still follows the material, so cutouts and blended
        // surfaces keep their shape.
        outColor = vec4(solidSurfaceColor(), alpha);
        outMotionReactive = motionReactive(material.surface.y > 1.5 ? alpha : 0.0);
        return;
    }
    vec3 albedo = sampledBase.rgb * material.baseColor.rgb;
    vec4 orm = sampleMaterialTexture(material.textures.z, texCoord);
    float metallic = clamp(orm.b * material.factors.x, 0.0, 1.0);
    float roughness = clamp(orm.g * material.factors.y, 0.045, 1.0);
    float occlusion = mix(1.0, sampleMaterialTexture(material.textures.w, texCoord).r, material.factors.w);
    // Screen-space occlusion describes the opaque depth; blended surfaces in
    // front of it keep only their material occlusion.
    bool water = (material.extraTextures.y & LOBE_WATER) != 0u;
    bool transmissive = (material.extraTextures.y & LOBE_TRANSMISSION) != 0u || water;
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
    if (water) {
        normal = waterNormal(material.water, geometricNormal);
    }
    if ((material.extraTextures.y & LOBE_TERRAIN) != 0u) {
        TerrainSample ground = terrainSurface(material, geometricNormal);
        albedo = ground.albedo * material.baseColor.rgb;
        normal = ground.normal;
        roughness = clamp(ground.arm.g * material.factors.y, 0.045, 1.0);
        metallic = clamp(ground.arm.b * material.factors.x, 0.0, 1.0);
        occlusion = min(occlusion, mix(1.0, ground.arm.r, material.factors.w));
    }
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
    // Transmission replaces part of the diffuse base (transmittedRadiance);
    // water has no diffuse base, only what it scatters (waterTransmitted).
    vec3 diffuseAlbedo = water ? vec3(0.0) : albedo * (1.0 - lobes.transmission);
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
        vec3 localDirection;
        float localNDotL;
        vec3 localRadiance = localLightRadiance(light, normal, geometricNormal, localDirection, localNDotL);
        if (localNDotL <= 0.0) {
            continue;
        }
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
    // Water mirrors the scene around it where the screen shows it, and the
    // sky (the environment) elsewhere.
    if (water && PHYSICALLY_BASED_LIGHTING) {
        vec4 screenReflection = waterScreenReflection(normal, viewDirection);
        vec2 brdf = textureLod(environmentBrdf, vec2(nDotV, roughness), 0.0).rg;
        vec3 specular = specularWeight * (reflectance * brdf.x + brdf.y);
        indirect = mix(indirect, specular * screenReflection.rgb, screenReflection.a);
    }
    vec3 emissive = sampleMaterialTexture(material.extraTextures.x, texCoord).rgb * material.emissive.rgb;
    // Base and emission sit below the sheen, which sits below the coat.
    float baseScale = lobes.sheenAlbedoScaling * lobes.clearcoatAttenuation;
    vec3 color = (direct + indirect) * baseScale + emissive * lobes.clearcoatAttenuation + lobeLight;
    // The background already carries the fog in front of it, so transmitted
    // light joins after this surface's own fog.
    vec3 transmitted = vec3(0.0);
    if (water) {
        float coverage = 1.0;
        transmitted = waterTransmitted(material, normal, geometricNormal, viewDirection, nDotV, roughness,
                                       reflectance, specularWeight, coverage) *
                      baseScale;
        alpha = coverage;
    } else if ((lobes.flags & LOBE_TRANSMISSION) != 0u) {
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
    float reactive = max(material.surface.y > 1.5 ? alpha : 0.0,
                         smoothstep(1.0, 4.0, dot(emissive, vec3(0.2126, 0.7152, 0.0722))));
    // Moving waves have no motion vectors; less history keeps them sharp.
    if (water) {
        reactive = max(reactive, 0.5);
    }
    outMotionReactive = motionReactive(reactive);
}
