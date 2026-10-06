#pragma once

// The tunable parts of the "8-bit Blender" theme.

#include "editor/theme/EditorPalette.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// The tunable parts of the "8-bit Blender" theme. There is one look; these
// only adjust it. The defaults are the theme as designed. The editor session
// owns the values (from settings.ini) and hands them explicitly to whatever
// paints: applyEditorColors and ThemeChrome.

// The highlight colour of focus rings, active overlines, drag targets, and
// the gizmo selection. Every accent stands out from the panes, fields,
// buttons, and headers it is drawn on (see EditorPaletteTests).
enum class ThemeAccent : uint8_t {
    Blue,
    Orange,
    Teal,
    Green,
    Violet,
    Red,
};

// The largest UI scale override: screen pixels per art pixel.
inline constexpr uint32_t maximumUiScale = 4;

struct ThemeAccentInfo {
    ThemeAccent accent;
    // Stored in settings.ini, for example "blue".
    std::string_view key;
    std::string_view label;
    ThemeColor color;
};

// Every accent, the default (Blue, palette::accentBlue) first.
std::span<const ThemeAccentInfo> themeAccents();
const ThemeAccentInfo& themeAccentInfo(ThemeAccent accent);
std::optional<ThemeAccent> parseThemeAccent(std::string_view key);

struct ThemeParameters {
    ThemeAccent accent{ThemeAccent::Blue};

    bool operator==(const ThemeParameters&) const = default;
};

// A wash over the editor's chrome that shows a game is running. It is the
// session's runtime state, handed to ThemeChrome::setModeTint each frame and
// never saved with the parameters.
enum class ThemeModeTint : uint8_t {
    None,
    Play,
    Simulate,
};

// The tint's colour (palette::playTint or simulateTint), or
// nullopt for None.
std::optional<ThemeColor> themeModeTintColor(ThemeModeTint tint);

// The parameters with every value moved into its supported range: an unknown
// accent becomes the default.
ThemeParameters clampedThemeParameters(ThemeParameters parameters);
