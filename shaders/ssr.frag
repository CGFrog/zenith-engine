#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrScene;
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
layout(location = 0) out vec4 outReflection;

void main() {
    outReflection = vec4(0.0);
    if (settings.reflectionMode < 0.5) {
        return;
    }
    vec2 uv = texCoord * settings.sceneScale;
    float centerDepth = texture(sceneDepth, uv).r;
    // Reversed Z: 0 is the far plane (sky), and nearer surfaces are larger.
    if (centerDepth <= 0.00001) {
        return;
    }
    vec2 depthGradient = vec2(dFdx(centerDepth), dFdy(centerDepth));
    vec2 direction = normalize(depthGradient + vec2(0.00001));
    float stride = settings.reflectionMode > 1.5 ? 1.5 : 3.0;
    int stepCount = settings.reflectionMode > 1.5 ? 24 : 12;
    vec2 rayUv = uv;
    for (int stepIndex = 1; stepIndex <= stepCount; ++stepIndex) {
        rayUv += direction * settings.inverseResolution * stride;
        if (any(lessThan(rayUv, vec2(0.0))) || any(greaterThan(rayUv, settings.sceneScale))) {
            break;
        }
        float rayDepth = texture(sceneDepth, rayUv).r;
        float expectedDepth = centerDepth - float(stepIndex) * 0.0015;
        if (rayDepth > expectedDepth && rayDepth < centerDepth + 0.02) {
            vec2 normalizedUv = rayUv / settings.sceneScale;
            float edgeFade = smoothstep(0.0, 0.08,
                                        min(min(normalizedUv.x, normalizedUv.y),
                                            min(1.0 - normalizedUv.x, 1.0 - normalizedUv.y)));
            outReflection = vec4(texture(hdrScene, rayUv).rgb, edgeFade * 0.35);
            return;
        }
    }
    // A miss is transparent: the lit scene's probes and IBL remain visible.
}
