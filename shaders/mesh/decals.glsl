// Clustered decals blended into the surface before lighting (mesh.frag).
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_DECALS_GLSL
#define ZENITH_MESH_DECALS_GLSL

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

#endif
