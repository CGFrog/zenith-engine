// Decoding of the packed vertex streams (engine/assets/PackedVertexStreams.hpp).
#ifndef ZENITH_VERTEX_PACKING_GLSL
#define ZENITH_VERTEX_PACKING_GLSL

#include "octahedral.glsl"

// Mirrors vertex_packing::unpackTangent.
vec4 decodeTangent(vec2 packedTangent) {
    const float minimumFoldedMagnitude = 1.0 / 32767.0;
    float magnitude = (abs(packedTangent.y) - minimumFoldedMagnitude) / (1.0 - minimumFoldedMagnitude);
    vec2 encoded = vec2(packedTangent.x, clamp(magnitude * 2.0 - 1.0, -1.0, 1.0));
    return vec4(decodeOctahedral(encoded), packedTangent.y < 0.0 ? -1.0 : 1.0);
}

#endif
