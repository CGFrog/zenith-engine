// The emitter table record and the per-frame uniform block (set 0, binding 0)
// of the particle emit and simulate passes.
// Mirrors particles::GpuParticleEmitter (std430) and
// particles::GpuParticleFrame (std140) in engine/graphics/Particles.hpp.
#ifndef ZENITH_PARTICLE_SIMULATION_GLSL
#define ZENITH_PARTICLE_SIMULATION_GLSL

layout(set = 0, binding = 0) uniform ParticleFrame {
    mat4 viewProjection;
    mat4 lightViewProjections[3];
    vec4 cameraPosition; // w: time step
    vec4 lightDirection; // xyz: toward the sun
    vec4 lightRadiance;
    vec4 cascadeSplits;
    vec4 clusterDepth;   // x, y: slice = log(depth) * x + y; z, w: projection[0][0], [1][1]
    uvec4 clusterGrid;   // columns, rows, slices, local light count
    uvec4 counts;        // x: pool slots, y: spawns, z: sort keys, w: shadow cascades
    vec4 ambient;        // rgb: hemisphere ambient radiance, w: environment diffuse intensity
    uvec4 flags;         // x: ambient from the irradiance SH, y: emitter table entries
    vec4 irradianceVolumeMinimum;
    vec4 irradianceVolumeCellSize;
    uvec4 irradianceVolumeCounts;
} frame;

struct Emitter {
    mat4 localToWorld;
    vec4 gravityDrag;
    vec4 direction;
    vec4 speedLifetime;
    vec4 sizeSpin; // x, y: start and end size, z: angular speed
    vec4 startColor;
    vec4 endColor;
    vec4 shading; // x: emissive intensity, y: soft distance
    uvec4 pool;
    uvec4 spawn;
};

#endif
