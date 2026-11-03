// Integer hashes for shader noise.
#ifndef ZENITH_HASH_GLSL
#define ZENITH_HASH_GLSL

// PCG hash (Jarzynski and Olano, "Hash Functions for GPU Rendering").
uint pcgHash(uint value) {
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

#endif
