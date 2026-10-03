#version 450

// Writes the drawn object's pick id and the surface's reversed-Z device depth,
// which the readback returns beside the ids (RenderPicking.hpp).

layout(push_constant) uniform IdDraw {
    mat4 modelViewProjection;
    // x: first joint in the palette, y: joint count, z: object id.
    uvec4 object;
} draw;

layout(location = 0) out uint outId;
layout(location = 1) out float outDepth;

void main() {
    outId = draw.object.z;
    outDepth = gl_FragCoord.z;
}
