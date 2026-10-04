#include "editor/theme/ThemeParameters.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
// Hues already in the palette or the wallpaper, so any accent belongs to the
// same night scene.
constexpr std::array<ThemeAccentInfo, 6> accents{{
    {ThemeAccent::Mint, "mint", "Mint", palette::pixelGlow},
    {ThemeAccent::Aqua, "aqua", "Aqua", palette::info},
    {ThemeAccent::Lime, "lime", "Lime", ThemeColor::hex(0xD0FF60)},
    {ThemeAccent::Sky, "sky", "Sky", ThemeColor::hex(0x6CB8FF)},
    {ThemeAccent::Amber, "amber", "Amber", palette::activeObjectLabel},
    {ThemeAccent::Violet, "violet", "Violet", ThemeColor::hex(0xC8A8FF)},
}};
} // namespace

std::span<const ThemeAccentInfo> themeAccents() {
    return accents;
}

const ThemeAccentInfo& themeAccentInfo(ThemeAccent accent) {
    for (const ThemeAccentInfo& info : accents) {
        if (info.accent == accent) {
            return info;
        }
    }
    return accents.front();
}

std::optional<ThemeAccent> parseThemeAccent(std::string_view key) {
    for (const ThemeAccentInfo& info : accents) {
        if (info.key == key) {
            return info.accent;
        }
    }
    return std::nullopt;
}

ThemeParameters clampedThemeParameters(ThemeParameters parameters) {
    const ThemeParameters defaults;
    if (!std::isfinite(parameters.glassOpacity)) {
        parameters.glassOpacity = defaults.glassOpacity;
    }
    if (!std::isfinite(parameters.paneOpacity)) {
        parameters.paneOpacity = defaults.paneOpacity;
    }
    parameters.frostRadius = std::min(parameters.frostRadius, ThemeParameters::maximumFrostRadius);
    parameters.glassOpacity =
        std::clamp(parameters.glassOpacity, ThemeParameters::minimumGlassOpacity, ThemeParameters::maximumGlassOpacity);
    parameters.paneOpacity = std::clamp(parameters.paneOpacity, ThemeParameters::minimumPaneOpacity, 1.0f);
    parameters.accent = themeAccentInfo(parameters.accent).accent;
    return parameters;
}

std::optional<ThemeColor> themeModeTintColor(ThemeModeTint tint) {
    switch (tint) {
    case ThemeModeTint::None:
        return std::nullopt;
    case ThemeModeTint::Play:
        return palette::playTint;
    case ThemeModeTint::Simulate:
        return palette::simulateTint;
    }
    return std::nullopt;
}

ThemeColor paneColor(const ThemeParameters& parameters) {
    return palette::pane.withAlpha(parameters.paneOpacity);
}
