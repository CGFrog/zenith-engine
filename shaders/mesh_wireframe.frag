#version 450

// Wireframe view shading (ViewShading.hpp): triangle edges rasterized as lines
// over the Solid shading, in one dark colour. The pipeline writes only the
// scene colour's RGB; the motion target keeps the surface's vectors.

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

// Linear scene radiance, dark against the clay so edges read at 0 EV.
const vec3 WIRE_COLOR = vec3(0.02, 0.022, 0.026);

void main() {
    outColor = vec4(WIRE_COLOR, 1.0);
    outMotionReactive = vec4(0.0);
}
