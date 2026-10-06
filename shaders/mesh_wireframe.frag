#version 450

// Wireframe view shading (ViewShading.hpp): triangle edges rasterized as lines
// on the cleared scene, in one light colour, through every surface. The
// pipeline writes only the scene colour's RGB.

// mesh.vert's outputs, declared so the interface matches the shading pipelines.
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 texCoord;
layout(location = 3) in vec4 worldTangent;
layout(location = 4) in vec4 currentClipPosition;
layout(location = 5) in vec4 previousClipPosition;
layout(location = 6) flat in uint materialIndex;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outMotionReactive;

// Linear scene radiance, light against the dark clear colour so edges read at
// 0 EV.
const vec3 WIRE_COLOR = vec3(0.42, 0.44, 0.48);

void main() {
    outColor = vec4(WIRE_COLOR, 1.0);
    outMotionReactive = vec4(0.0);
}
