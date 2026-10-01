#include "EnvironmentLighting.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>

namespace {
constexpr float pi = 3.14159265358979f;

uint64_t contentHash(const TextureAsset& texture, uint32_t levels) {
    uint64_t hash = 1469598103934665603ULL;
    auto append = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    for (uint8_t value : texture.rgba) {
        append(value);
    }
    for (uint16_t value : texture.rgbaHalf) {
        append(static_cast<uint8_t>(value));
        append(static_cast<uint8_t>(value >> 8));
    }
    for (uint32_t value : {texture.width, texture.height, levels}) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            append(static_cast<uint8_t>(value >> shift));
        }
    }
    return hash;
}

// Reads one texel as linear light; false when the texel is missing.
bool linearTexel(const TextureAsset& texture, size_t texel, glm::vec3& color) {
    const size_t offset = texel * 4;
    if (!texture.rgbaHalf.empty()) {
        if (offset + 2 >= texture.rgbaHalf.size()) {
            return false;
        }
        color =
            glm::vec3(glm::unpackHalf1x16(texture.rgbaHalf[offset]), glm::unpackHalf1x16(texture.rgbaHalf[offset + 1]),
                      glm::unpackHalf1x16(texture.rgbaHalf[offset + 2]));
        return true;
    }
    if (offset + 2 >= texture.rgba.size()) {
        return false;
    }
    color = glm::pow(glm::vec3(texture.rgba[offset], texture.rgba[offset + 1], texture.rgba[offset + 2]) / 255.0f,
                     glm::vec3(2.2f));
    return true;
}

// Real spherical-harmonic basis through band 2, in the order the GLSL uses.
std::array<float, 9> shBasis(const glm::vec3& direction) {
    const float x = direction.x;
    const float y = direction.y;
    const float z = direction.z;
    return {0.282095f,
            0.488603f * y,
            0.488603f * z,
            0.488603f * x,
            1.092548f * x * y,
            1.092548f * y * z,
            0.315392f * (3.0f * z * z - 1.0f),
            1.092548f * x * z,
            0.546274f * (x * x - y * y)};
}

// Clamped-cosine convolution per band divided by pi (A0 = pi, A1 = 2pi/3, A2 = pi/4).
constexpr std::array<float, 9> cosineBandFactors{1.0f,  2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f,
                                                 0.25f, 0.25f,       0.25f,       0.25f};

float cubeAreaElement(float x, float y) {
    return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0f));
}

float radicalInverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

// GGX half vector around +Z for a low-discrepancy sample.
glm::vec3 importanceSampleGgx(uint32_t index, uint32_t count, float roughness) {
    const float alpha = roughness * roughness;
    const float phi = 2.0f * pi * static_cast<float>(index) / static_cast<float>(count);
    const float xi = radicalInverse(index);
    const float cosTheta = std::sqrt((1.0f - xi) / (1.0f + (alpha * alpha - 1.0f) * xi));
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
    return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

float smithGeometryIbl(float nDotV, float nDotL, float roughness) {
    const float k = roughness * roughness * 0.5f;
    return nDotV / (nDotV * (1.0f - k) + k) * (nDotL / (nDotL * (1.0f - k) + k));
}
} // namespace

const PrefilteredEnvironment& EnvironmentLightingCache::preprocess(const TextureAsset& texture,
                                                                   uint32_t roughnessLevels) {
    roughnessLevels = std::max(roughnessLevels, 1u);
    const uint64_t hash = contentHash(texture, roughnessLevels);
    if (const auto found = m_entries.find(hash); found != m_entries.end()) {
        return found->second;
    }
    PrefilteredEnvironment result;
    result.sourceHash = hash;
    glm::vec3 sky(0.0f);
    glm::vec3 ground(0.0f);
    uint64_t skySamples = 0;
    uint64_t groundSamples = 0;
    for (uint32_t y = 0; y < texture.height; ++y) {
        for (uint32_t x = 0; x < texture.width; ++x) {
            glm::vec3 color(0.0f);
            if (!linearTexel(texture, static_cast<size_t>(y) * texture.width + x, color)) {
                continue;
            }
            if (y < texture.height / 2) {
                sky += color;
                ++skySamples;
            } else {
                ground += color;
                ++groundSamples;
            }
        }
    }
    result.diffuseSky = skySamples == 0 ? glm::vec3(0.0f) : sky / static_cast<float>(skySamples);
    result.diffuseGround = groundSamples == 0 ? result.diffuseSky : ground / static_cast<float>(groundSamples);
    const glm::vec3 average = (result.diffuseSky + result.diffuseGround) * 0.5f;
    for (uint32_t level = 0; level < roughnessLevels; ++level) {
        const float roughness = roughnessLevels == 1 ? 1.0f : static_cast<float>(level) / (roughnessLevels - 1);
        result.specularRoughnessLevels.push_back(glm::mix(result.diffuseSky, average, roughness));
    }
    return m_entries.emplace(hash, std::move(result)).first->second;
}

namespace environment_lighting {
glm::vec3 cubeFaceDirection(uint32_t face, const glm::vec2& uv) {
    const float u = uv.x * 2.0f - 1.0f;
    const float v = uv.y * 2.0f - 1.0f;
    glm::vec3 direction(0.0f);
    switch (face) {
    case 0:
        direction = {1.0f, -v, -u};
        break;
    case 1:
        direction = {-1.0f, -v, u};
        break;
    case 2:
        direction = {u, 1.0f, v};
        break;
    case 3:
        direction = {u, -1.0f, -v};
        break;
    case 4:
        direction = {u, -v, 1.0f};
        break;
    default:
        direction = {-u, -v, -1.0f};
        break;
    }
    return glm::normalize(direction);
}

CubeFaceCoordinate cubeFaceCoordinate(const glm::vec3& direction) {
    // Vulkan's cube face selection table (major axis, then sc / tc).
    const glm::vec3 magnitude = glm::abs(direction);
    uint32_t face = 0;
    float major = 1.0f;
    glm::vec2 coordinate(0.0f);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
        face = direction.x >= 0.0f ? 0u : 1u;
        major = magnitude.x;
        coordinate = {direction.x >= 0.0f ? -direction.z : direction.z, -direction.y};
    } else if (magnitude.y >= magnitude.z) {
        face = direction.y >= 0.0f ? 2u : 3u;
        major = magnitude.y;
        coordinate = {direction.x, direction.y >= 0.0f ? direction.z : -direction.z};
    } else {
        face = direction.z >= 0.0f ? 4u : 5u;
        major = magnitude.z;
        coordinate = {direction.z >= 0.0f ? direction.x : -direction.x, -direction.y};
    }
    return {face, (coordinate / std::max(major, 1e-20f) + 1.0f) * 0.5f};
}

glm::vec2 equirectangularCoordinate(const glm::vec3& direction) {
    const glm::vec3 unit = glm::normalize(direction);
    return {std::atan2(unit.z, unit.x) / (2.0f * pi) + 0.5f, std::acos(std::clamp(unit.y, -1.0f, 1.0f)) / pi};
}

glm::vec3 equirectangularDirection(const glm::vec2& uv) {
    const float phi = (uv.x - 0.5f) * 2.0f * pi;
    const float theta = uv.y * pi;
    return {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
}

glm::vec3 proceduralSkyRadiance(const glm::vec3& direction, const glm::vec3& skyColor, const glm::vec3& groundColor) {
    const glm::vec3 horizon = glm::mix(skyColor, groundColor, 0.35f) + skyColor * 0.25f;
    const float height = glm::normalize(direction).y;
    if (height >= 0.0f) {
        return glm::mix(horizon, skyColor, glm::smoothstep(0.0f, 0.45f, height));
    }
    return glm::mix(horizon, groundColor, glm::smoothstep(0.0f, 0.12f, -height));
}

IrradianceCoefficients projectIrradiance(const RadianceFunction& radiance, uint32_t faceSize) {
    faceSize = std::max(faceSize, 1u);
    IrradianceCoefficients coefficients{};
    float totalWeight = 0.0f;
    const float texelSize = 2.0f / static_cast<float>(faceSize);
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < faceSize; ++y) {
            for (uint32_t x = 0; x < faceSize; ++x) {
                const float x0 = static_cast<float>(x) * texelSize - 1.0f;
                const float y0 = static_cast<float>(y) * texelSize - 1.0f;
                const float x1 = x0 + texelSize;
                const float y1 = y0 + texelSize;
                const float weight = cubeAreaElement(x0, y0) - cubeAreaElement(x0, y1) - cubeAreaElement(x1, y0) +
                                     cubeAreaElement(x1, y1);
                const glm::vec2 uv = (glm::vec2(x, y) + 0.5f) / static_cast<float>(faceSize);
                const glm::vec3 direction = cubeFaceDirection(face, uv);
                const glm::vec3 sample = radiance(direction) * weight;
                const std::array<float, 9> basis = shBasis(direction);
                for (size_t index = 0; index < basis.size(); ++index) {
                    coefficients[index] += sample * basis[index];
                }
                totalWeight += weight;
            }
        }
    }
    // The texel solid angles sum to 4 pi analytically; renormalizing removes the
    // remaining floating-point error.
    const float normalization = totalWeight > 0.0f ? 4.0f * pi / totalWeight : 0.0f;
    for (size_t index = 0; index < coefficients.size(); ++index) {
        coefficients[index] *= normalization * cosineBandFactors[index];
    }
    return coefficients;
}

glm::vec3 evaluateIrradiance(const IrradianceCoefficients& coefficients, const glm::vec3& normal) {
    const std::array<float, 9> basis = shBasis(glm::normalize(normal));
    glm::vec3 result(0.0f);
    for (size_t index = 0; index < basis.size(); ++index) {
        result += coefficients[index] * basis[index];
    }
    return glm::max(result, glm::vec3(0.0f));
}

glm::vec2 integrateEnvironmentBrdf(float nDotV, float roughness, uint32_t sampleCount) {
    nDotV = std::clamp(nDotV, 1e-4f, 1.0f);
    sampleCount = std::max(sampleCount, 1u);
    const glm::vec3 view(std::sqrt(1.0f - nDotV * nDotV), 0.0f, nDotV);
    glm::vec2 result(0.0f);
    for (uint32_t index = 0; index < sampleCount; ++index) {
        const glm::vec3 halfVector = importanceSampleGgx(index, sampleCount, roughness);
        const glm::vec3 light = 2.0f * glm::dot(view, halfVector) * halfVector - view;
        const float nDotL = std::max(light.z, 0.0f);
        if (nDotL <= 0.0f) {
            continue;
        }
        const float nDotH = std::max(halfVector.z, 0.0f);
        const float vDotH = std::max(glm::dot(view, halfVector), 0.0f);
        const float visibility = smithGeometryIbl(nDotV, nDotL, roughness) * vDotH / std::max(nDotH * nDotV, 1e-6f);
        const float fresnel = std::pow(1.0f - vDotH, 5.0f);
        result += glm::vec2((1.0f - fresnel) * visibility, fresnel * visibility);
    }
    return result / static_cast<float>(sampleCount);
}

float prefilteredMipRoughness(uint32_t mip, uint32_t mipCount) {
    return mipCount <= 1 ? 0.0f : static_cast<float>(mip) / static_cast<float>(mipCount - 1);
}
} // namespace environment_lighting
