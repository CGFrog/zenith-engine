#version 450
#extension GL_GOOGLE_include_directive : require

// Writes the drawn object's pick id and the surface's reversed-Z device depth,
// which the readback returns beside the ids (RenderPicking.hpp).

#include "id_draw.glsl"

layout(location = 0) out uint outId;
layout(location = 1) out float outDepth;

void main() {
    outId = draw.object.z;
    outDepth = gl_FragCoord.z;
}
