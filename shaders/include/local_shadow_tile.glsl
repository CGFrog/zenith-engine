// One tile of the local-light shadow atlas.
// Mirrors GpuLocalShadowTile in engine/graphics/passes/LocalShadowPass.hpp
// (std430).
#ifndef ZENITH_LOCAL_SHADOW_TILE_GLSL
#define ZENITH_LOCAL_SHADOW_TILE_GLSL

struct LocalShadowTile {
    mat4 viewProjection; // world to the tile's clip space
    vec4 atlasRect;      // uv of the tile's top-left corner (xy) and its uv extent (zw)
    vec4 params;         // x: shadow-texel world size per unit distance from the light
};

#endif
