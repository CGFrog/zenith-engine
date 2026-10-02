#include "ParticleEmitter.hpp"

#include <cmath>
#include <numbers>

namespace {
bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finiteNonNegative(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

bool validColor(const glm::vec4& color) {
    return finiteNonNegative(color.r) && finiteNonNegative(color.g) && finiteNonNegative(color.b) &&
           finiteNonNegative(color.a) && color.a <= 1.0f;
}
} // namespace

std::string validateParticleEmitter(const ParticleEmitterComponent& emitter) {
    if (emitter.maxParticles == 0 || emitter.maxParticles > maximumParticlesPerEmitter) {
        return "Particle emitter maximum must be between 1 and 1048576 particles";
    }
    if (!finiteNonNegative(emitter.spawnRate) || !finiteNonNegative(emitter.burstInterval)) {
        return "Particle emitter spawn rate and burst interval must be finite and non-negative";
    }
    if (!(emitter.lifetimeMin > 0.0f) || !std::isfinite(emitter.lifetimeMax) ||
        emitter.lifetimeMax < emitter.lifetimeMin) {
        return "Particle lifetimes must be positive with the maximum at least the minimum";
    }
    if (!finite(emitter.direction) || glm::dot(emitter.direction, emitter.direction) < 1e-8f) {
        return "Particle emitter direction must be a finite non-zero vector";
    }
    if (!(emitter.coneAngleRadians >= 0.0f && emitter.coneAngleRadians <= std::numbers::pi_v<float>)) {
        return "Particle emission cone angle must be between 0 and pi";
    }
    if (!finiteNonNegative(emitter.speedMin) || !std::isfinite(emitter.speedMax) ||
        emitter.speedMax < emitter.speedMin) {
        return "Particle speeds must be non-negative with the maximum at least the minimum";
    }
    if (!finiteNonNegative(emitter.spawnRadius) || !finite(emitter.gravity) || !finiteNonNegative(emitter.drag) ||
        !finiteNonNegative(emitter.angularSpeed)) {
        return "Particle spawn radius, gravity, drag, or angular speed is invalid";
    }
    if (!finiteNonNegative(emitter.startSize) || !finiteNonNegative(emitter.endSize)) {
        return "Particle sizes must be finite and non-negative";
    }
    if (!validColor(emitter.startColor) || !validColor(emitter.endColor)) {
        return "Particle colors must be non-negative with opacity between 0 and 1";
    }
    if (!finiteNonNegative(emitter.emissiveIntensity) || !finiteNonNegative(emitter.softDistance)) {
        return "Particle emissive intensity and soft distance must be finite and non-negative";
    }
    return {};
}
