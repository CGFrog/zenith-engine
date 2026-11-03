// Terrain surfaces (LOBE_TERRAIN) for mesh.frag: material layers blended by
// vertex weight and tiled in world space.
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_TERRAIN_GLSL
#define ZENITH_MESH_TERRAIN_GLSL

// Layers blended by vertex weight, tiled in world space. The last layer is
// projected from three axes (reoriented UDN normals) so it does not stretch on
// cliffs; the others are projected from above.
struct TerrainSample {
    vec3 albedo;
    vec3 normal;
    vec3 arm; // ambient occlusion, roughness, metallic
};

vec3 terrainTangentNormal(uint slot, vec2 uv) {
    vec2 xy = sampleMaterialTexture(slot, uv).xy * 2.0 - 1.0;
    return vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
}

// Seen from above: u runs along +X and the image's up (+Y of an OpenGL normal
// map) along -Z, around the geometric normal.
TerrainSample terrainFromAbove(MaterialRecord material, int layer, vec3 geometricNormal) {
    vec2 uv = worldPosition.xz / material.terrainTileSize[layer];
    vec3 mapped = terrainTangentNormal(material.terrainNormal[layer], uv);
    TerrainSample result;
    result.albedo = sampleMaterialTexture(material.terrainAlbedo[layer], uv).rgb;
    result.arm = sampleMaterialTexture(material.terrainArm[layer], uv).rgb;
    result.normal = normalize(geometricNormal + vec3(mapped.x, 0.0, -mapped.y));
    return result;
}

TerrainSample terrainTriplanar(MaterialRecord material, int layer, vec3 geometricNormal) {
    vec3 blend = pow(abs(geometricNormal), vec3(4.0));
    blend /= max(blend.x + blend.y + blend.z, 1.0e-5);
    float tile = material.terrainTileSize[layer];
    vec2 uvX = worldPosition.zy / tile;
    vec2 uvY = worldPosition.xz / tile;
    vec2 uvZ = worldPosition.xy / tile;
    uint albedoSlot = material.terrainAlbedo[layer];
    uint armSlot = material.terrainArm[layer];
    uint normalSlot = material.terrainNormal[layer];
    TerrainSample result;
    result.albedo = sampleMaterialTexture(albedoSlot, uvX).rgb * blend.x +
                    sampleMaterialTexture(albedoSlot, uvY).rgb * blend.y +
                    sampleMaterialTexture(albedoSlot, uvZ).rgb * blend.z;
    result.arm = sampleMaterialTexture(armSlot, uvX).rgb * blend.x + sampleMaterialTexture(armSlot, uvY).rgb * blend.y +
                 sampleMaterialTexture(armSlot, uvZ).rgb * blend.z;
    vec3 tangentX = terrainTangentNormal(normalSlot, uvX);
    vec3 tangentY = terrainTangentNormal(normalSlot, uvY);
    vec3 tangentZ = terrainTangentNormal(normalSlot, uvZ);
    vec3 normalX = vec3(tangentX.xy + geometricNormal.zy, abs(tangentX.z) * geometricNormal.x);
    vec3 normalY = vec3(tangentY.xy + geometricNormal.xz, abs(tangentY.z) * geometricNormal.y);
    vec3 normalZ = vec3(tangentZ.xy + geometricNormal.xy, abs(tangentZ.z) * geometricNormal.z);
    result.normal = normalize(normalX.zyx * blend.x + normalY.xzy * blend.y + normalZ.xyz * blend.z);
    return result;
}

// The blended surface. Each layer's brightness acts as its height, so where
// layers meet the higher detail (stones, grass tufts) shows through first
// instead of a soft cross-fade.
TerrainSample terrainSurface(MaterialRecord material, vec3 geometricNormal) {
    vec4 weights = max(surfaceWeights, vec4(0.0));
    weights /= max(weights.x + weights.y + weights.z + weights.w, 1.0e-4);
    TerrainSample layers[4];
    vec4 heights = vec4(0.0);
    for (int layer = 0; layer < 4; ++layer) {
        if (weights[layer] <= 0.001) {
            layers[layer] = TerrainSample(vec3(0.0), geometricNormal, vec3(1.0, 1.0, 0.0));
            continue;
        }
        layers[layer] = layer == 3 ? terrainTriplanar(material, layer, geometricNormal)
                                   : terrainFromAbove(material, layer, geometricNormal);
        heights[layer] = dot(layers[layer].albedo, vec3(0.333));
    }
    const float transition = 0.2;
    vec4 raised = weights + heights * 0.6 * step(vec4(0.001), weights);
    float top = max(max(raised.x, raised.y), max(raised.z, raised.w));
    vec4 blend = max(raised - (top - transition), vec4(0.0)) * step(vec4(0.001), weights);
    blend /= max(blend.x + blend.y + blend.z + blend.w, 1.0e-4);
    TerrainSample result = TerrainSample(vec3(0.0), vec3(0.0), vec3(0.0));
    for (int layer = 0; layer < 4; ++layer) {
        result.albedo += layers[layer].albedo * blend[layer];
        result.normal += layers[layer].normal * blend[layer];
        result.arm += layers[layer].arm * blend[layer];
    }
    result.normal = normalize(result.normal);
    return result;
}

#endif
