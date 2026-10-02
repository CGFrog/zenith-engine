#version 450

// Camera-facing particle quads pulled from the sorted keys: instance i draws
// the particle named by key i, so instances follow the back-to-front order.
// See src/engine/graphics/Particles.hpp and ParticlePass.cpp.

// Prefix of CameraData in mesh.frag (std140); only these members are read.
layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
} camera;

// Mirrors particles::GpuParticle (std430).
struct Particle {
    vec4 positionAge;
    vec4 velocityLifetime;
    vec4 worldPositionSize; // xyz: world center, w: size
    vec4 radiance;          // rgb: radiance, a: opacity
    vec4 render;            // x: rotation, y: soft distance
    uvec4 info;             // z: flags and texture slot
};

layout(std430, set = 1, binding = 0) readonly buffer Particles {
    Particle values[];
} particles;

layout(std430, set = 1, binding = 1) readonly buffer SortKeys {
    uvec2 values[];
} sortKeys;

// World-space camera axes spanning the view plane.
layout(push_constant) uniform Billboard {
    vec4 right;
    vec4 up;
} billboard;

layout(location = 0) out vec2 spriteUv;
layout(location = 1) out vec4 particleRadiance;
layout(location = 2) flat out uint particleFlags;
layout(location = 3) flat out float softDistance;

const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0),
                                vec2(-1.0, 1.0));

void main() {
    Particle particle = particles.values[sortKeys.values[gl_InstanceIndex].y];
    vec2 corner = corners[gl_VertexIndex];
    float cosine = cos(particle.render.x);
    float sine = sin(particle.render.x);
    vec2 rotated = vec2(cosine * corner.x - sine * corner.y, sine * corner.x + cosine * corner.y);
    vec3 offset = (billboard.right.xyz * rotated.x + billboard.up.xyz * rotated.y) * (0.5 * particle.worldPositionSize.w);
    gl_Position = camera.viewProjection * vec4(particle.worldPositionSize.xyz + offset, 1.0);
    spriteUv = vec2(corner.x, -corner.y) * 0.5 + 0.5;
    particleRadiance = particle.radiance;
    particleFlags = particle.info.z;
    softDistance = particle.render.y;
}
