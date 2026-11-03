// Octahedral direction encoding. octahedralEncode mirrors the
// irradiance_volume namespace in engine/graphics/IrradianceVolume.cpp;
// decodeOctahedral mirrors vertex_packing::decodeOctahedral and also reads
// the depth prepass normals.
#ifndef ZENITH_OCTAHEDRAL_GLSL
#define ZENITH_OCTAHEDRAL_GLSL

vec2 signNotZero(vec2 value) {
    return vec2(value.x >= 0.0 ? 1.0 : -1.0, value.y >= 0.0 ? 1.0 : -1.0);
}

// Y-up octahedral map of a unit direction to [0, 1]^2 (irradiance probe depth).
vec2 octahedralEncode(vec3 direction) {
    vec2 projected = direction.xz / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    if (direction.y < 0.0) {
        projected = (1.0 - abs(projected.yx)) * signNotZero(projected);
    }
    return projected * 0.5 + 0.5;
}

// Z-up octahedral [-1, 1]^2 back to a unit direction (packed vertex normals
// and tangents, prepass normals).
vec3 decodeOctahedral(vec2 encoded) {
    vec3 direction = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    float fold = max(-direction.z, 0.0);
    direction.x += direction.x >= 0.0 ? -fold : fold;
    direction.y += direction.y >= 0.0 ? -fold : fold;
    return normalize(direction);
}

#endif
