#pragma once

// Validates and clamps RenderSettings against device capabilities.

#include "Rendering.hpp"

#include <string>
#include <string_view>
#include <vector>

struct RenderConfigurationResult {
    bool success{false};
    std::string error;
    explicit operator bool() const { return success; }
};

struct ValidatedRenderSettings {
    RenderSettings settings;
    std::vector<std::string> fallbacks;
};

RenderConfigurationResult serializeRenderSettings(const RenderSettings& settings, std::string& output);
RenderConfigurationResult deserializeRenderSettings(std::string_view input, RenderSettings& settings);
ValidatedRenderSettings validateRenderSettings(const RenderSettings& requested, const RenderCapabilities& capabilities);
