#pragma once

#include "engine/assets/Assets.hpp"

#include <glm/glm.hpp>
#include <span>
#include <unordered_map>
#include <vector>

struct PrefilteredEnvironment {
    glm::vec3 diffuseSky{0.0f};
    glm::vec3 diffuseGround{0.0f};
    std::vector<glm::vec3> specularRoughnessLevels;
    uint64_t sourceHash{0};
};

// Deterministic CPU preprocessing used by tools and as the compatible runtime
// fallback. The result is cached by content, not by source path.
class EnvironmentLightingCache {
  public:
    const PrefilteredEnvironment& preprocess(const TextureAsset& equirectangular, uint32_t roughnessLevels = 6);
    size_t entryCount() const { return m_entries.size(); }
    void clear() { m_entries.clear(); }

  private:
    std::unordered_map<uint64_t, PrefilteredEnvironment> m_entries;
};
