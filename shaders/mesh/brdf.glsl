// The GGX microfacet BRDF and the base layer's response to one light
// (mesh.frag).
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_BRDF_GLSL
#define ZENITH_MESH_BRDF_GLSL

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

#endif
