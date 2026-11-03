#version 450
#extension GL_GOOGLE_include_directive : require

#include "camera.glsl"

// World-space point on the far plane, kept homogeneous so it interpolates linearly.
layout(location = 0) out vec4 farPoint;
layout(location = 1) out vec2 currentNdc;

void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    // Reversed Z puts the far plane at depth 0, the scene depth clear value.
    vec4 clip = vec4(position, 0.0, 1.0);
    farPoint = inverse(camera.viewProjection) * clip;
    currentNdc = position;
    gl_Position = clip;
}
