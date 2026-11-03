// Push constants of the object-ID pipeline used for picking.
// Mirrors IdBufferDraw in engine/graphics/passes/IdBufferPass.hpp.
#ifndef ZENITH_ID_DRAW_GLSL
#define ZENITH_ID_DRAW_GLSL

layout(push_constant) uniform IdDraw {
    mat4 modelViewProjection;
    // x: first joint in the palette, y: joint count, z: object id.
    uvec4 object;
} draw;

#endif
