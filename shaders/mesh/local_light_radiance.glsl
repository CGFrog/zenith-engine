// Light from one clustered local light at the fragment (mesh.frag).
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_LOCAL_LIGHT_RADIANCE_GLSL
#define ZENITH_MESH_LOCAL_LIGHT_RADIANCE_GLSL

// Radiance arriving from a local light (range-attenuated, cone-faded for
// spots, and shadowed), with its direction and its cosine at the shaded
// normal. A light that cannot reach the fragment gives a zero cosine.
vec3 localLightRadiance(LocalLight light, vec3 normal, vec3 geometricNormal, out vec3 direction, out float nDotL) {
    direction = vec3(0.0);
    nDotL = 0.0;
    vec3 toLight = light.positionRange.xyz - worldPosition;
    float distanceToLight = length(toLight);
    float range = light.positionRange.w;
    if (distanceToLight >= range || distanceToLight <= 0.0001) {
        return vec3(0.0);
    }
    direction = toLight / distanceToLight;
    float attenuation =
        pow(max(1.0 - distanceToLight / range, 0.0), 2.0) / max(distanceToLight * distanceToLight, 0.01);
    if (light.directionType.w > 0.5) {
        float coneCosine = dot(-direction, normalize(light.directionType.xyz));
        attenuation *= smoothstep(light.spotCone.y, light.spotCone.x, coneCosine);
    }
    float cosine = max(dot(normal, direction), 0.0);
    if (cosine <= 0.0 || attenuation <= 0.0) {
        return vec3(0.0);
    }
    nDotL = cosine;
    attenuation *= localShadowVisibility(light, worldPosition, geometricNormal, direction, distanceToLight);
    return light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
}

#endif
