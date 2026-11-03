// Directional cascaded shadows and local-light atlas shadows for mesh.frag.
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_SHADOWS_GLSL
#define ZENITH_MESH_SHADOWS_GLSL

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
    // Interleaved gradient noise (Jimenez 2014) over screen pixels rotates the
    // kernel differently at every pixel. A hash of the world position's xz
    // was constant along walls and lost precision far from the origin, which
    // drew the filter's error as stripes; TAA averages this pattern out.
    float angle = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) * 6.28318530718;
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
        // Clamped so the zero-cascade specialization, where this branch is
        // dead, still indexes in range (spirv-val rejects a constant -1).
        float lastSplit = camera.cascadeSplits[max(cascadeCount - 1, 0)];
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

#endif
