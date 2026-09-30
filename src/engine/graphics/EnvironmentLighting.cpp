#include "EnvironmentLighting.hpp"

#include <algorithm>

namespace {
uint64_t contentHash(const TextureAsset& texture, uint32_t levels) {
    uint64_t hash = 1469598103934665603ULL;
    auto append = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    for (uint8_t value : texture.rgba) append(value);
    for (uint32_t value : {texture.width, texture.height, levels}) {
        for (uint32_t shift = 0; shift < 32; shift += 8) append(static_cast<uint8_t>(value >> shift));
    }
    return hash;
}
}

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
            const size_t offset = (static_cast<size_t>(y) * texture.width + x) * 4;
            if (offset + 2 >= texture.rgba.size()) continue;
            const glm::vec3 color = glm::pow(glm::vec3(texture.rgba[offset], texture.rgba[offset + 1],
                                                       texture.rgba[offset + 2]) /
                                                 255.0f,
                                             glm::vec3(2.2f));
            if (y < texture.height / 2) { sky += color; ++skySamples; }
            else { ground += color; ++groundSamples; }
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
