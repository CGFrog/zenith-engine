#pragma once

#include "../assets/Assets.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

// How a particle's color combines with what is behind it. Alpha particles are
// sorted back to front with every other particle; additive ones only add
// light, so they ignore their own coverage.
enum class ParticleBlendMode : uint8_t {
    Alpha,
    Additive,
};

// World particles keep their position once spawned; Local particles are
// simulated in the emitter's space and follow the entity transform.
enum class ParticleSimulationSpace : uint8_t {
    World,
    Local,
};

// GPU particle emitter, drawn when RenderSettings::particles is not Off (see
// engine/graphics/Particles.hpp for the renderer side). Distances are world
// units (meters), times are seconds, angles radians, and colors linear.
// Particles spawn inside a sphere around the entity origin and leave along a
// cone around `direction` (entity space). Each particle's color, alpha, and
// size interpolate from their start to their end value over its lifetime.
struct ParticleEmitterComponent {
    // Most live particles of this emitter; once reached, each new particle
    // replaces the oldest one. The renderer's quality budget may lower it.
    uint32_t maxParticles{1024};
    // Continuous emission rate (particles per second).
    float spawnRate{32.0f};
    // Particles spawned at once when the emitter starts and, with a positive
    // burstInterval, again every burstInterval seconds.
    uint32_t burstCount{0};
    float burstInterval{0.0f};
    float lifetimeMin{1.0f};
    float lifetimeMax{2.0f};
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    // Half angle of the emission cone; pi emits in every direction.
    float coneAngleRadians{0.35f};
    float speedMin{1.0f};
    float speedMax{2.0f};
    float spawnRadius{0.0f};
    // World-space acceleration, for example (0, -9.81, 0) for falling sparks
    // or a small positive y for rising smoke.
    glm::vec3 gravity{0.0f};
    // Linear drag: velocity decays as exp(-drag * t).
    float drag{0.0f};
    // Billboard edge length at birth and at death.
    float startSize{0.2f};
    float endSize{0.2f};
    // Each particle spins at a random rate in [-angularSpeed, angularSpeed].
    float angularSpeed{0.0f};
    // rgb: linear color (albedo when lit); a: opacity.
    glm::vec4 startColor{1.0f};
    glm::vec4 endColor{1.0f, 1.0f, 1.0f, 0.0f};
    // Self-illumination: emitted radiance is color * emissiveIntensity.
    float emissiveIntensity{1.0f};
    // Lit particles also reflect the sun, local lights, and ambient light
    // reaching their center (see particles::ParticleLightingModel).
    bool lit{false};
    ParticleBlendMode blendMode{ParticleBlendMode::Alpha};
    ParticleSimulationSpace space{ParticleSimulationSpace::World};
    // Fade distance against opaque geometry behind a particle (soft
    // particles); zero draws hard intersections.
    float softDistance{0.25f};
    // Optional sprite (rgb tints the color, a scales opacity). Empty uses the
    // procedural soft round sprite.
    std::string textureAsset;
    TextureHandle texture{};
    // Disabled emitters stop spawning; live particles finish their lifetimes.
    bool emitting{true};
};

// Upper bound on ParticleEmitterComponent::maxParticles.
inline constexpr uint32_t maximumParticlesPerEmitter = 1u << 20;

// Empty when the emitter can be simulated, otherwise why it cannot.
std::string validateParticleEmitter(const ParticleEmitterComponent& emitter);
