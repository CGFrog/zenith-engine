// Ambient lighting of the base layer for mesh.frag: image-based lighting,
// or the flat hemisphere fallback when it is off.
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_AMBIENT_LIGHTING_GLSL
#define ZENITH_MESH_AMBIENT_LIGHTING_GLSL

// Lagarde's specular occlusion from ambient occlusion: 1 when unoccluded, 0
// when fully occluded. Mirrors ambient_occlusion::specularOcclusion.
float specularOcclusion(float nDotV, float occlusion, float roughness) {
    return clamp(pow(nDotV + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion, 0.0, 1.0);
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

#endif
