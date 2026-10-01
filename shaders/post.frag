#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrScene;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D temporalHistory;
layout(set = 0, binding = 3) uniform sampler2D motionReactive;
layout(set = 0, binding = 4) uniform sampler2D ambientOcclusion;
layout(set = 0, binding = 5) uniform sampler2D screenSpaceReflections;
// Previous frame's motion target; .w holds its reversed-Z depth.
layout(set = 0, binding = 6) uniform sampler2D previousMotionDepth;
// Mip 0 of the bloom chain (BloomExposurePass); covers the valid scene region.
layout(set = 0, binding = 7) uniform sampler2D bloomChain;
// Adapted exposure written by exposure_adapt.comp.
layout(std430, set = 0, binding = 8) readonly buffer ExposureState {
    float exposure;
    float averageLogLuminance;
    float targetExposure;
    float padding;
} exposureState;

layout(push_constant) uniform PostSettings {
    mat4 inverseViewProjection;
    mat4 previousViewProjection;
    vec4 cameraPosition;
    vec4 grid;
    vec2 inverseResolution;
    vec2 sceneScale;
    float bloomStrength;
    float antialiasingMode;
    float temporalHistoryValid;
    float ambientOcclusionMode;
    float reflectionMode;
    float tonemapOperator;
    vec2 bloomUvScale;
    vec2 bloomUvLimit;
} settings;

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

float luminance(vec3 color) { return dot(color, vec3(0.2126, 0.7152, 0.0722)); }

vec3 acesToneMap(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

// AgX (Troy Sobotka) with the polynomial fit of Blender's sigmoid. Mirrors
// toneMap in engine/graphics/ToneMapping.cpp. The output target is an sRGB
// format, so AgX's display-encoded result is decoded to linear with its 2.2
// reference exponent and the hardware re-encodes it on store.
vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 agxToneMap(vec3 color, bool punchy) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minimumEv = -12.47393;
    const float maximumEv = 4.026069;
    vec3 encoded = inset * max(color, vec3(0.0));
    encoded = clamp(log2(max(encoded, vec3(1e-10))), minimumEv, maximumEv);
    encoded = agxContrast((encoded - minimumEv) / (maximumEv - minimumEv));
    if (punchy) {
        float luma = luminance(encoded);
        encoded = luma + 1.4 * (pow(max(encoded, vec3(0.0)), vec3(1.35)) - luma);
    }
    return pow(clamp(outset * encoded, 0.0, 1.0), vec3(2.2));
}

vec3 toneMap(vec3 color) {
    if (settings.tonemapOperator < 0.5) {
        return acesToneMap(color);
    }
    return agxToneMap(color, settings.tonemapOperator > 1.5);
}

vec4 editorGrid(vec2 uv) {
    if (settings.grid.w < 0.5) {
        return vec4(0.0);
    }
    vec2 ndc = uv * 2.0 - 1.0;
    // Reversed Z: depth 1 is the near plane and 0 the far plane.
    vec4 nearPoint = settings.inverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec4 farPoint = settings.inverseViewProjection * vec4(ndc, 0.0, 1.0);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    vec3 rayDirection = normalize(farPoint.xyz - nearPoint.xyz);
    if (abs(rayDirection.y) < 0.00001) {
        return vec4(0.0);
    }
    float intersection = (settings.grid.x - settings.cameraPosition.y) / rayDirection.y;
    if (intersection <= 0.0) {
        return vec4(0.0);
    }
    vec3 worldPosition = settings.cameraPosition.xyz + rayDirection * intersection;
    float sampledDepth = texture(sceneDepth, uv * settings.sceneScale).r;
    if (sampledDepth > 0.000001) {
        vec4 sceneWorld = settings.inverseViewProjection * vec4(ndc, sampledDepth, 1.0);
        sceneWorld /= sceneWorld.w;
        float sceneDistance = length(sceneWorld.xyz - settings.cameraPosition.xyz);
        if (intersection > sceneDistance + settings.grid.y * 0.02) {
            return vec4(0.0);
        }
    }

    vec2 minorCoordinates = worldPosition.xz / settings.grid.y;
    vec2 minorWidth = max(fwidth(minorCoordinates), vec2(0.0001));
    vec2 minorDistance = abs(fract(minorCoordinates - 0.5) - 0.5) / minorWidth;
    float minorLine = 1.0 - min(min(minorDistance.x, minorDistance.y), 1.0);
    vec2 majorCoordinates = minorCoordinates / 10.0;
    vec2 majorWidth = max(fwidth(majorCoordinates), vec2(0.0001));
    vec2 majorDistance = abs(fract(majorCoordinates - 0.5) - 0.5) / majorWidth;
    float majorLine = 1.0 - min(min(majorDistance.x, majorDistance.y), 1.0);
    float xAxis = 1.0 - smoothstep(0.0, fwidth(worldPosition.z) * 1.5, abs(worldPosition.z));
    float zAxis = 1.0 - smoothstep(0.0, fwidth(worldPosition.x) * 1.5, abs(worldPosition.x));

    vec3 color = mix(vec3(0.22, 0.23, 0.27), vec3(0.42, 0.43, 0.48), majorLine);
    color = mix(color, vec3(0.85, 0.12, 0.10), xAxis);
    color = mix(color, vec3(0.08, 0.25, 0.90), zAxis);
    float line = max(max(minorLine * 0.35, majorLine * 0.65), max(xAxis, zAxis));
    float fade = 1.0 - smoothstep(settings.grid.z * 0.6, settings.grid.z,
                                  distance(worldPosition.xz, settings.cameraPosition.xz));
    return vec4(color, line * fade * 0.85);
}

// True when the surface now at this pixel was hidden behind nearer geometry in
// the previous frame, so its history belongs to a different surface. Only camera
// motion is reprojected; pixels whose motion vector shows their own movement
// fall back to the neighborhood clamp.
bool disoccluded(vec2 uv, vec2 motion, float currentDepth) {
    vec4 world = settings.inverseViewProjection * vec4(uv * 2.0 - 1.0, currentDepth, 1.0);
    world /= world.w;
    vec4 previousClip = settings.previousViewProjection * world;
    if (previousClip.w <= 0.0) {
        return true;
    }
    vec2 cameraPreviousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
    vec2 objectMotionPixels = (uv - cameraPreviousUv - motion) / settings.inverseResolution;
    if (dot(objectMotionPixels, objectMotionPixels) > 0.25) {
        return false;
    }
    float expectedDepth = previousClip.z / previousClip.w;
    vec4 previousDepths = textureGather(previousMotionDepth, (uv - motion) * settings.sceneScale, 3);
    float nearestPrevious = max(max(previousDepths.x, previousDepths.y), max(previousDepths.z, previousDepths.w));
    // Reversed-Z depth is proportional to 1 / view distance, so this tolerance is
    // a 2% relative distance regardless of range.
    return nearestPrevious > expectedDepth * 1.02;
}

void main() {
    vec2 sceneTexCoord = texCoord * settings.sceneScale;
    vec3 center = texture(hdrScene, sceneTexCoord).rgb;
    if (settings.antialiasingMode > 1.5 && settings.temporalHistoryValid > 0.5) {
        vec4 motion = texture(motionReactive, sceneTexCoord);
        vec2 previousUv = texCoord - motion.xy;
        bool historyOnScreen = all(greaterThanEqual(previousUv, vec2(0.0))) &&
                               all(lessThanEqual(previousUv, vec2(1.0)));
        if (historyOnScreen) {
            vec3 neighborhoodMinimum = center;
            vec3 neighborhoodMaximum = center;
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    vec3 sampleColor = texture(hdrScene, sceneTexCoord + vec2(x, y) * settings.inverseResolution).rgb;
                    neighborhoodMinimum = min(neighborhoodMinimum, sampleColor);
                    neighborhoodMaximum = max(neighborhoodMaximum, sampleColor);
                }
            }
            vec3 history = clamp(texture(temporalHistory, previousUv * settings.sceneScale).rgb,
                                 neighborhoodMinimum, neighborhoodMaximum);
            float motionPixels = length((texCoord - previousUv) / settings.inverseResolution);
            float reactive = max(motion.z, smoothstep(0.25, 2.0, abs(luminance(center) - luminance(history))));
            if (disoccluded(texCoord, motion.xy, motion.w)) {
                reactive = 1.0;
            }
            float historyWeight = 0.92 * (1.0 - reactive) * (1.0 - clamp(motionPixels / 32.0, 0.0, 0.75));
            center = mix(center, history, historyWeight);
        }
    }
    vec2 ambientOcclusionUv = sceneTexCoord * (settings.ambientOcclusionMode > 0.5 && settings.ambientOcclusionMode < 1.5 ? 0.5 : 1.0);
    vec2 reflectionUv = sceneTexCoord * (settings.reflectionMode > 0.5 && settings.reflectionMode < 1.5 ? 0.5 : 1.0);
    center *= texture(ambientOcclusion, ambientOcclusionUv).r;
    vec4 reflection = texture(screenSpaceReflections, reflectionUv);
    center = mix(center, reflection.rgb, reflection.a);
    vec4 grid = editorGrid(texCoord);
    // Grid colors are display-referred; pre-divide so exposure leaves them unchanged.
    center = mix(center, grid.rgb / max(exposureState.exposure, 1e-6), grid.a);

    vec3 antialiased = center;
    if (settings.antialiasingMode > 0.5 && settings.antialiasingMode < 1.5) {
        float centerLuma = luminance(center);
        float northLuma = luminance(texture(hdrScene, sceneTexCoord + vec2(0.0, -settings.inverseResolution.y)).rgb);
        float southLuma = luminance(texture(hdrScene, sceneTexCoord + vec2(0.0, settings.inverseResolution.y)).rgb);
        float westLuma = luminance(texture(hdrScene, sceneTexCoord + vec2(-settings.inverseResolution.x, 0.0)).rgb);
        float eastLuma = luminance(texture(hdrScene, sceneTexCoord + vec2(settings.inverseResolution.x, 0.0)).rgb);
        float minimumLuma = min(centerLuma, min(min(northLuma, southLuma), min(westLuma, eastLuma)));
        float maximumLuma = max(centerLuma, max(max(northLuma, southLuma), max(westLuma, eastLuma)));
        float contrast = maximumLuma - minimumLuma;
        float threshold = max(0.0312, maximumLuma * 0.125);
        if (contrast > threshold) {
            vec2 edgeDirection = vec2(-(northLuma - southLuma), westLuma - eastLuma);
            float directionScale = max(abs(edgeDirection.x), abs(edgeDirection.y));
            if (directionScale > 0.0001) {
                edgeDirection = edgeDirection / directionScale * settings.inverseResolution;
                vec3 first = texture(hdrScene, sceneTexCoord + edgeDirection * 0.5).rgb;
                vec3 second = texture(hdrScene, sceneTexCoord - edgeDirection * 0.5).rgb;
                antialiased = (first + second) * 0.5;
            }
        }
    }
    // Exposure scales the scene and bloom alike; bloom is an energy-conserving mix.
    vec3 exposed = antialiased * exposureState.exposure;
    if (settings.bloomStrength > 0.0) {
        vec3 bloom = texture(bloomChain, min(texCoord * settings.bloomUvScale, settings.bloomUvLimit)).rgb;
        exposed = mix(exposed, bloom * exposureState.exposure, settings.bloomStrength);
    }
    outColor = vec4(toneMap(exposed), 1.0);
}
