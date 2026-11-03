#version 450
#extension GL_GOOGLE_include_directive : require

#include "debug_line.glsl"

layout(location = 0) in vec3 position;
// Display-referred (sRGB-encoded) colour.
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 outColor;

vec3 srgbToLinear(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), greaterThan(encoded, vec3(0.04045)));
}

void main() {
    gl_Position = settings.viewProjection * vec4(position, 1.0);
    // An sRGB target re-encodes on store, so the authored colour is decoded first.
    outColor = settings.target.z > 0.5 ? vec4(srgbToLinear(color.rgb), color.a) : color;
}
