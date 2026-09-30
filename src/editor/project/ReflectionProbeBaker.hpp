#pragma once

#include "engine/graphics/EnvironmentLighting.hpp"

#include <filesystem>
#include <string>

struct ReflectionProbeBakeSettings {
    uint32_t roughnessLevels{6};
};

struct ReflectionProbeBakeResult {
    bool success{false};
    uint64_t sourceHash{0};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Editor-owned policy for deterministic probe preprocessing. Runtime code only
// consumes environment data and never decides when or where a probe is baked.
class ReflectionProbeBaker {
  public:
    ReflectionProbeBakeResult bake(const TextureAsset& source, const std::filesystem::path& destination,
                                   ReflectionProbeBakeSettings settings = {});

  private:
    EnvironmentLightingCache m_cache;
};
