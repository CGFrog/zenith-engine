// One simulated particle and the particle flags.
// Mirrors particles::GpuParticle and the particles::flag* bits in
// engine/graphics/Particles.hpp (std430).
#ifndef ZENITH_PARTICLE_GLSL
#define ZENITH_PARTICLE_GLSL

const uint FLAG_ADDITIVE = 1u;
const uint FLAG_LIT = 2u;
const uint FLAG_LOCAL_SPACE = 4u;

struct Particle {
    vec4 positionAge;
    vec4 velocityLifetime;
    vec4 worldPositionSize; // xyz: world center, w: size
    vec4 radiance;          // rgb: radiance, a: opacity
    vec4 render;            // x: rotation, y: soft distance
    uvec4 info;             // z: flags and texture slot
};

#endif
