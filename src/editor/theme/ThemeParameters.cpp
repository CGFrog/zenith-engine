#include "editor/theme/ThemeParameters.hpp"

#include <array>

namespace {
// Bright enough to read as lines and rings on every grey surface; Orange is
// the viewport's selection hue.
constexpr std::array<ThemeAccentInfo, 6> accents{{
    {ThemeAccent::Blue, "blue", "Blue", palette::accentBlue},
    {ThemeAccent::Orange, "orange", "Orange", palette::overlaySelected},
    {ThemeAccent::Teal, "teal", "Teal", ThemeColor::hex(0x3CC8B4)},
    {ThemeAccent::Green, "green", "Green", palette::success},
    {ThemeAccent::Violet, "violet", "Violet", ThemeColor::hex(0xA882F0)},
    {ThemeAccent::Red, "red", "Red", ThemeColor::hex(0xF05A5A)},
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
