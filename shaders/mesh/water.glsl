// Water surfaces (LOBE_WATER) for mesh.frag: animated wave normals, light
// through the water, and screen-space reflections of the opaque scene.
// Part of mesh.frag: reads its bindings, stage inputs and specialization
// constants, and the parts included before it.
#ifndef ZENITH_MESH_WATER_GLSL
#define ZENITH_MESH_WATER_GLSL

// The surface is a sum of directional waves, each moving at the deep-water
// speed of its length (omega = sqrt(g k)). Only their slopes are needed: the
// waves tilt the normal, which bends the reflections, the sun's glints, and
// the refracted view of the bed.
const int WATER_WAVES = 6;

// Surface slope (dh/dx, dh/dz) at world xz. Waves much shorter than a pixel
// fade out, so distant water stays calm instead of shimmering.
vec2 waterSlope(vec2 position, vec4 water, float time, float pixelSize) {
    const vec2 directions[WATER_WAVES] =
        vec2[WATER_WAVES](vec2(1.0, 0.0), vec2(0.62, 0.78), vec2(-0.35, 0.94), vec2(0.89, -0.45),
                          vec2(-0.83, -0.56), vec2(0.21, -0.98));
    const float lengths[WATER_WAVES] = float[WATER_WAVES](1.0, 0.61, 0.37, 0.23, 0.14, 0.085);
    vec2 slope = vec2(0.0);
    for (int wave = 0; wave < WATER_WAVES; ++wave) {
        float waveLength = water.x * lengths[wave];
        float k = 6.28318530718 / waveLength;
        float omega = sqrt(9.81 * k) * water.z;
        float phase = k * dot(directions[wave], position) - omega * time + float(wave) * 1.7;
        // Steepness is the slope amplitude; shorter waves are gentler.
        float amplitude = water.y * 0.45 / (1.0 + float(wave) * 0.35);
        float detail = clamp(waveLength / (pixelSize * 6.0) - 0.5, 0.0, 1.0);
        slope += directions[wave] * (amplitude * detail * cos(phase));
    }
    return slope;
}

// The wavy normal of an upward-facing water surface; other faces (the sides
// of a water volume) keep their geometric normal.
vec3 waterNormal(vec4 water, vec3 geometricNormal) {
    if (geometricNormal.y < 0.5) {
        return geometricNormal;
    }
    float pixelSize = max(length(fwidth(worldPosition)), 1.0e-4);
    vec2 slope = waterSlope(worldPosition.xz, water, camera.surfaceAnimation.x, pixelSize);
    return normalize(vec3(-slope.x, 1.0, -slope.y));
}

// View-space depth of a reversed-Z depth value (see clusterDepth).
float viewDepthOf(float depth) {
    return camera.clusterDepth.z / (depth + camera.clusterDepth.w);
}

float opaqueViewDepth(vec2 pixel) {
    vec2 extent = 1.0 / camera.fogScreen.xy;
    ivec2 texel = ivec2(clamp(pixel, vec2(0.0), extent - 1.0));
    return viewDepthOf(texelFetch(sceneDepth, texel, 0).r);
}

// Light reaching the eye up through the water, and the surface's coverage,
// which fades its edge where the water is shallow. The opaque depth behind
// the surface gives the path length through the water: the bed shows through
// shallow water and absorbs into the scattered water color with depth.
vec3 waterTransmitted(MaterialRecord material, vec3 normal, vec3 geometricNormal, vec3 viewDirection, float nDotV,
                      float roughness, vec3 reflectance, float specularWeight, out float coverage) {
    vec2 pixel = gl_FragCoord.xy;
    float surfaceDepth = viewDepthOf(gl_FragCoord.z);
    float distanceToSurface = length(camera.cameraPosition.xyz - worldPosition);
    // Along the view ray, then straight down.
    float pathLength = material.volume.w;
    float waterDepth = material.volume.w;
    bool measured = camera.surfaceAnimation.y > 0.5;
    if (measured) {
        float behind = max(opaqueViewDepth(pixel) - surfaceDepth, 0.0);
        pathLength = behind * distanceToSurface / max(surfaceDepth, 1.0e-4);
        waterDepth = pathLength * abs(viewDirection.y);
    }
    // Refraction: the waves shift the view of the bed, more through deeper
    // water and less far away. A shifted sample that lands on something in
    // front of the water would smear it onto the surface, so it falls back.
    // In pixels: up to 4% of the render height for full-depth water nearby.
    float shiftPixels = clamp(waterDepth * 4.0, 0.0, 1.0) * 0.04 /
                        (camera.fogScreen.y * max(distanceToSurface * 0.3, 1.0));
    vec2 refractedPixel = pixel + (normal.xz - geometricNormal.xz) * shiftPixels;
    if (measured && opaqueViewDepth(refractedPixel) < surfaceDepth) {
        refractedPixel = pixel;
    }
    vec2 uv = refractedPixel * camera.fogScreen.xy;
    vec3 background = textureLod(transmissionBackground, uv, 0.0).rgb;
    vec3 absorption = exp(-material.volume.rgb * pathLength);
    // In-scattering: the water's own color, lit by the sky and the sun
    // (which reaches into the water along its depth).
    vec3 lightDirection = normalize(camera.lightDirection.xyz);
    vec3 skyLight = camera.environmentSky.rgb * camera.environmentSky.w;
    vec3 sunLight = camera.lightColor.rgb * camera.lightDirection.w * max(lightDirection.y, 0.0) * 0.06;
    vec3 scattered = material.baseColor.rgb * (skyLight + sunLight) * (vec3(1.0) - absorption);
    vec3 reflected = vec3(0.0);
    if (PHYSICALLY_BASED_LIGHTING) {
        vec2 brdf = textureLod(environmentBrdf, vec2(nDotV, roughness), 0.0).rg;
        reflected = specularWeight * (reflectance * brdf.x + brdf.y);
    }
    coverage = measured && material.water.w > 0.0 ? smoothstep(0.0, material.water.w, waterDepth) : 1.0;
    return (vec3(1.0) - reflected) * (background * absorption + scattered);
}

// Reflections of the opaque scene on water: the reflected ray marches in
// world space with growing steps; each step projects to the screen and is
// compared with the opaque depth there, and the first step that passes
// behind it takes the opaque scene's color. rgb is that color, a its weight,
// which fades toward the screen edges and the end of the march (where the
// sky reflection takes over). The engine's screen-space reflections trace
// opaque surfaces only, before blended surfaces draw.
vec4 waterScreenReflection(vec3 normal, vec3 viewDirection) {
    if (camera.surfaceAnimation.y < 0.5) {
        return vec4(0.0);
    }
    const int steps = 40;
    vec3 direction = reflect(-viewDirection, normal);
    // Waves can tip a grazing reflection below the horizon; keep it above
    // the surface so it never marches into the water.
    direction = normalize(vec3(direction.x, max(direction.y, 0.02), direction.z));
    float distanceToSurface = length(camera.cameraPosition.xyz - worldPosition);
    float stepLength = clamp(distanceToSurface * 0.03, 0.03, 0.5);
    vec3 position = worldPosition;
    vec2 extent = 1.0 / camera.fogScreen.xy;
    for (int index = 0; index < steps; ++index) {
        position += direction * stepLength;
        stepLength *= 1.12;
        vec4 clip = camera.viewProjection * vec4(position, 1.0);
        if (clip.w <= 0.0) {
            break;
        }
        vec3 projected = clip.xyz / clip.w;
        vec2 uv = projected.xy * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            break;
        }
        float behind = viewDepthOf(projected.z) - opaqueViewDepth(uv * extent);
        // Passing just behind a surface is a hit; far behind is a ray
        // slipping past the back of something closer.
        if (behind > 0.0 && behind < stepLength * 3.0 + 0.2) {
            vec2 edges = smoothstep(vec2(0.0), vec2(0.1), uv) * smoothstep(vec2(0.0), vec2(0.1), 1.0 - uv);
            float weight = edges.x * edges.y * (1.0 - smoothstep(0.6, 1.0, float(index) / float(steps)));
            return vec4(textureLod(transmissionBackground, uv, 0.0).rgb, weight);
        }
    }
    return vec4(0.0);
}

#endif
