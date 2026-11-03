// The KHR material extension lobes layered over metallic-roughness in
// mesh.frag: specular, sheen, clearcoat and transmission.
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_SURFACE_LOBES_GLSL
#define ZENITH_MESH_SURFACE_LOBES_GLSL

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

#include "sheen.glsl"

float sheenAlpha(float sheenRoughness) {
    return max(sheenRoughness * sheenRoughness, 1e-6);
}

#include "dielectric_specular.glsl"

// tangentFrame maps tangent space to world space around the geometric normal.
SurfaceLobes surfaceLobes(MaterialRecord material, vec2 uv, mat3 tangentFrame, vec3 viewDirection, float nDotV) {
    SurfaceLobes lobes;
    // The fast lighting variant keeps only transmission, so glass stays
    // see-through; sheen and the coat need the physically based model.
    const uint fastLobes = LOBE_TRANSMISSION | LOBE_VOLUME | LOBE_WATER;
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

#endif
