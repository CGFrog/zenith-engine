#pragma once

#include "editor/theme/EditorPalette.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// The tunable parts of the "Pixel Aero Night" theme. There is one look; these
// only adjust it. The defaults are the theme as designed. The editor session
// owns the values (from settings.ini) and hands them explicitly to whatever
// paints: applyEditorColors, ThemeChrome, and the glass backdrop.

// The highlight colour of focus rings, active overlines, drag targets, and
// the gizmo selection. Every accent is a palette hue and stands out from the
// panes, fields, and glass it is drawn on (see EditorPaletteTests).
enum class ThemeAccent : uint8_t {
    Mint,
    Aqua,
    Lime,
    Sky,
    Amber,
    Violet,
};

// The largest UI scale override: screen pixels per art pixel.
inline constexpr uint32_t maximumUiScale = 4;

struct ThemeAccentInfo {
    ThemeAccent accent;
    // Stored in settings.ini, for example "mint".
    std::string_view key;
    std::string_view label;
    ThemeColor color;
};

// Every accent, the default (Mint, palette::pixelGlow) first.
std::span<const ThemeAccentInfo> themeAccents();
const ThemeAccentInfo& themeAccentInfo(ThemeAccent accent);
std::optional<ThemeAccent> parseThemeAccent(std::string_view key);

struct ThemeParameters {
    // Wallpaper cells the frosted glass blurs over; 0 shows it sharp.
    static constexpr uint32_t maximumFrostRadius = 6;
    // How far the glass mixes the blurred wallpaper toward palette::glassTint.
    static constexpr float minimumGlassOpacity = 0.15f;
    static constexpr float maximumGlassOpacity = 0.75f;
    // Panes stay opaque enough that every text colour keeps 4.5:1 over the
    // brightest wallpaper colour behind them.
    static constexpr float minimumPaneOpacity = 0.9f;

    // Paints the procedural wallpaper behind the editor. Off, the desktop is
    // the flat night sky and glass is the flat fallback fill.
    bool wallpaper{true};
    uint32_t frostRadius{3};
    float glassOpacity{palette::glassTintAmount};
    // Opacity of the solid content panes inside the glass frames.
    float paneOpacity{1.0f};
    ThemeAccent accent{ThemeAccent::Mint};

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

// The tint's colour (palette::playTint or palette::simulateTint), or nullopt
// for None.
std::optional<ThemeColor> themeModeTintColor(ThemeModeTint tint);

// The parameters with every value moved into its supported range.
ThemeParameters clampedThemeParameters(ThemeParameters parameters);
// The pane colour as painted with the parameters' opacity.
ThemeColor paneColor(const ThemeParameters& parameters);
