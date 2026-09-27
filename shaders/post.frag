#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrScene;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(push_constant) uniform PostSettings {
    mat4 inverseViewProjection;
    vec4 cameraPosition;
    vec4 grid;
    vec2 inverseResolution;
    float bloomStrength;
    float antialiasingMode;
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

vec4 editorGrid(vec2 uv) {
    if (settings.grid.w < 0.5) {
        return vec4(0.0);
    }
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 nearPoint = settings.inverseViewProjection * vec4(ndc, 0.0, 1.0);
    vec4 farPoint = settings.inverseViewProjection * vec4(ndc, 1.0, 1.0);
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
    float sampledDepth = texture(sceneDepth, uv).r;
    if (sampledDepth < 0.999999) {
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

void main() {
    vec3 center = texture(hdrScene, texCoord).rgb;
    vec4 grid = editorGrid(texCoord);
    center = mix(center, grid.rgb, grid.a);
    vec3 bloom = vec3(0.0);
    if (settings.bloomStrength > 0.0) {
        vec3 downsampled = textureLod(hdrScene, texCoord, 2.0).rgb;
        bloom = max(downsampled - vec3(1.0), vec3(0.0)) * settings.bloomStrength;
    }

    vec3 antialiased = center;
    if (settings.antialiasingMode > 0.5) {
        float centerLuma = luminance(center);
        float northLuma = luminance(texture(hdrScene, texCoord + vec2(0.0, -settings.inverseResolution.y)).rgb);
        float southLuma = luminance(texture(hdrScene, texCoord + vec2(0.0, settings.inverseResolution.y)).rgb);
        float westLuma = luminance(texture(hdrScene, texCoord + vec2(-settings.inverseResolution.x, 0.0)).rgb);
        float eastLuma = luminance(texture(hdrScene, texCoord + vec2(settings.inverseResolution.x, 0.0)).rgb);
        float minimumLuma = min(centerLuma, min(min(northLuma, southLuma), min(westLuma, eastLuma)));
        float maximumLuma = max(centerLuma, max(max(northLuma, southLuma), max(westLuma, eastLuma)));
        float contrast = maximumLuma - minimumLuma;
        float threshold = max(0.0312, maximumLuma * 0.125);
        if (contrast > threshold) {
            vec2 edgeDirection = vec2(-(northLuma - southLuma), westLuma - eastLuma);
            float directionScale = max(abs(edgeDirection.x), abs(edgeDirection.y));
            if (directionScale > 0.0001) {
                edgeDirection = edgeDirection / directionScale * settings.inverseResolution;
                vec3 first = texture(hdrScene, texCoord + edgeDirection * 0.5).rgb;
                vec3 second = texture(hdrScene, texCoord - edgeDirection * 0.5).rgb;
                antialiased = (first + second) * 0.5;
            }
        }
    }
    outColor = vec4(acesToneMap(antialiased + bloom), 1.0);
}
