#version 450

// Mirrors debug_line.vert.
layout(push_constant) uniform DebugLineSettings {
    mat4 viewProjection;
    vec4 depthTerms;
    vec4 target;
} settings;

// The view's scene depth (reversed Z) within its render extent.
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;

layout(location = 0) in vec4 color;
layout(location = 0) out vec4 outColor;

void main() {
    if (settings.depthTerms.w < 0.5) {
        // The scene may render below output resolution, so its depth is
        // fetched at the texel covering this output pixel and compared as a
        // distance; the relative bias keeps lines on surfaces visible.
        ivec2 texel = ivec2(gl_FragCoord.xy * settings.target.xy);
        texel = min(texel, textureSize(sceneDepth, 0) - 1);
        float depth = texelFetch(sceneDepth, texel, 0).r;
        float sceneDistance = settings.depthTerms.y / (depth + settings.depthTerms.x);
        // gl_FragCoord.w is 1 / clip w, and clip w is the forward distance.
        float lineDistance = 1.0 / gl_FragCoord.w;
        if (lineDistance > sceneDistance * (1.0 + settings.depthTerms.z)) {
            discard;
        }
    }
    outColor = color;
}
