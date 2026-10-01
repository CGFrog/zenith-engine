#version 450

layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

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
} settings;

layout(location = 0) in vec2 texCoord;
layout(location = 0) out float outOcclusion;

void main() {
    if (settings.ambientOcclusionMode < 0.5) {
        outOcclusion = 1.0;
        return;
    }
    vec2 uv = texCoord * settings.sceneScale;
    float centerDepth = texture(sceneDepth, uv).r;
    // Reversed Z: 0 is the far plane (sky), and nearer surfaces are larger.
    if (centerDepth <= 0.00001) {
        outOcclusion = 1.0;
        return;
    }
    const vec2 directions[8] = vec2[8](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1),
                                       vec2(0.707, 0.707), vec2(-0.707, 0.707),
                                       vec2(0.707, -0.707), vec2(-0.707, -0.707));
    int radius = settings.ambientOcclusionMode > 1.5 ? 4 : 2;
    float occlusion = 0.0;
    for (int sampleIndex = 0; sampleIndex < 8; ++sampleIndex) {
        float sampledDepth = texture(sceneDepth, uv + directions[sampleIndex] * settings.inverseResolution * radius).r;
        float delta = sampledDepth - centerDepth;
        occlusion += smoothstep(0.0005, 0.012, delta) * (1.0 - smoothstep(0.02, 0.12, abs(delta)));
    }
    outOcclusion = 1.0 - occlusion * (settings.ambientOcclusionMode > 1.5 ? 0.065 : 0.045);
}
