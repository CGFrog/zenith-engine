#pragma once

#include <array>
#include <cstdint>

// One sRGB-encoded colour with straight alpha, every component in [0, 1].
struct ThemeColor {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};

    static constexpr ThemeColor hex(uint32_t rgb, float alpha = 1.0f) {
        return {static_cast<float>((rgb >> 16u) & 0xffu) / 255.0f, static_cast<float>((rgb >> 8u) & 0xffu) / 255.0f,
                static_cast<float>(rgb & 0xffu) / 255.0f, alpha};
    }

    constexpr ThemeColor withAlpha(float alpha) const {
        return {r, g, b, alpha};
    }

    bool operator==(const ThemeColor&) const = default;
};

// WCAG 2 relative luminance of an opaque sRGB colour (alpha ignored).
float relativeLuminance(ThemeColor color);
// WCAG 2 contrast ratio between two opaque colours, from 1 to 21.
float contrastRatio(ThemeColor first, ThemeColor second);
// The opaque colour of over drawn on top of an opaque under.
ThemeColor composite(ThemeColor over, ThemeColor under);

// The editor's "Pixel Aero Night" theme: Frutiger Aero glass-over-nature
// rendered as palette-limited pixel art. Every colour the editor draws comes
// from here; panels and widgets never write colour literals.
namespace palette {
// The procedural wallpaper's colours. Every wallpaper pixel is one of these.
inline constexpr std::array<ThemeColor, 21> wallpaper{
    ThemeColor::hex(0x01040E), ThemeColor::hex(0x030A1E), ThemeColor::hex(0x061536), ThemeColor::hex(0x0A2350),
    ThemeColor::hex(0x103466), ThemeColor::hex(0x1A4A7A), ThemeColor::hex(0x0C3A4A), ThemeColor::hex(0x13606A),
    ThemeColor::hex(0x1E8A80), ThemeColor::hex(0x2FC0A0), ThemeColor::hex(0x5EF0C0), ThemeColor::hex(0xB6FFE6),
    ThemeColor::hex(0xFFFFFF), ThemeColor::hex(0xFFF2B0), ThemeColor::hex(0xD8E4FF), ThemeColor::hex(0x021A16),
    ThemeColor::hex(0x04302A), ThemeColor::hex(0x0A4A40), ThemeColor::hex(0x3A1E6A), ThemeColor::hex(0x6A3AA0),
    ThemeColor::hex(0xD0FF60),
};

// Frosted glass: the blurred wallpaper mixed toward glassTint, or the
// fallback fill while no wallpaper texture is available.
inline constexpr ThemeColor glassTint = ThemeColor::hex(0x0C3A4A);
inline constexpr float glassTintAmount = 0.35f;
inline constexpr ThemeColor glassFallback = ThemeColor::hex(0x0C2A36);
inline constexpr ThemeColor glassFallbackDither = ThemeColor::hex(0x103440);
inline constexpr ThemeColor glassHighlight = ThemeColor::hex(0x96FFE6, 0.32f);
inline constexpr ThemeColor glassShade = ThemeColor::hex(0x000000, 0.45f);
inline constexpr ThemeColor glassBorder = ThemeColor::hex(0x00060C);
inline constexpr ThemeColor glassGlint = ThemeColor::hex(0xFFFFFF, 0.14f);
inline constexpr ThemeColor glassGlintFaint = ThemeColor::hex(0xFFFFFF, 0.09f);
// Glass over the 3D viewport, which cannot sample the wallpaper.
inline constexpr ThemeColor smokedGlass = ThemeColor::hex(0x0A2832, 0.62f);
inline constexpr ThemeColor smokedHighlight = ThemeColor::hex(0x96FFE6, 0.30f);
// Caption text drawn on glass, outlined so it reads over any wallpaper cell.
inline constexpr ThemeColor captionInk = ThemeColor::hex(0xE6FFF7);
inline constexpr ThemeColor captionOutline = ThemeColor::hex(0x020C14);

// Solid content panes inside the glass frames.
inline constexpr ThemeColor pane = ThemeColor::hex(0x081C2A);
inline constexpr ThemeColor paneShadow = ThemeColor::hex(0x040F18);
inline constexpr ThemeColor paneAlternate = ThemeColor::hex(0x0B2232);
inline constexpr ThemeColor popup = ThemeColor::hex(0x0A2230, 0.98f);
inline constexpr ThemeColor ink = ThemeColor::hex(0xD4EFE8);
inline constexpr ThemeColor inkMuted = ThemeColor::hex(0x7AA89C);
inline constexpr ThemeColor inkDisabled = ThemeColor::hex(0x5E8A80);
inline constexpr ThemeColor separator = ThemeColor::hex(0x1E4A50);

// Split-gloss buttons: a hard step between the top and bottom halves.
inline constexpr ThemeColor buttonTop = ThemeColor::hex(0x24605E);
inline constexpr ThemeColor buttonBottom = ThemeColor::hex(0x123E44);
inline constexpr ThemeColor buttonHighlight = ThemeColor::hex(0x4F9A8E);
inline constexpr ThemeColor buttonHoverTop = ThemeColor::hex(0x22726A);
inline constexpr ThemeColor buttonHoverBottom = ThemeColor::hex(0x186660);
inline constexpr ThemeColor buttonActiveTop = ThemeColor::hex(0xB0FFE0);
inline constexpr ThemeColor buttonActiveBottom = ThemeColor::hex(0x1FB08A);
inline constexpr ThemeColor activeInk = ThemeColor::hex(0x05261C);
// The 1-art-pixel ring around hovered and active controls.
inline constexpr ThemeColor pixelGlow = ThemeColor::hex(0x35FFB0);

// Recessed fields.
inline constexpr ThemeColor field = ThemeColor::hex(0x051824);
inline constexpr ThemeColor fieldHovered = ThemeColor::hex(0x082434);
inline constexpr ThemeColor fieldActive = ThemeColor::hex(0x0A2C3C);
inline constexpr ThemeColor textSelection = ThemeColor::hex(0x17625A);

// Collapsible section headers.
inline constexpr ThemeColor sectionTop = ThemeColor::hex(0x174A50);
inline constexpr ThemeColor sectionBottom = ThemeColor::hex(0x0D3038);
inline constexpr ThemeColor sectionInk = ThemeColor::hex(0x9FF5D5);

// List rows.
inline constexpr ThemeColor rowSelected = ThemeColor::hex(0x114440);
inline constexpr ThemeColor rowSelectedHighlight = ThemeColor::hex(0x38A48C);
inline constexpr ThemeColor rowSelectedShade = ThemeColor::hex(0x08342E);
inline constexpr ThemeColor rowHovered = ThemeColor::hex(0x0E3040);
inline constexpr ThemeColor indentGuide = ThemeColor::hex(0x2A6A68);

// Sliders, checkboxes and scrollbars.
inline constexpr ThemeColor sliderFill = ThemeColor::hex(0x1FB08A);
inline constexpr ThemeColor sliderHandle = ThemeColor::hex(0xB0FFE0);
inline constexpr ThemeColor scrollbarThumb = ThemeColor::hex(0x24605E);
inline constexpr ThemeColor scrollbarThumbHovered = ThemeColor::hex(0x22726A);
inline constexpr ThemeColor scrollbarThumbActive = ThemeColor::hex(0x35FFB0);

// Message severities, as text and icon colours.
inline constexpr ThemeColor success = ThemeColor::hex(0x35FFB0);
inline constexpr ThemeColor info = ThemeColor::hex(0x5EF0E0);
inline constexpr ThemeColor warning = ThemeColor::hex(0xFFC857);
inline constexpr ThemeColor error = ThemeColor::hex(0xFF8C8C);

// World axes, shared by the gizmo, modal transform guides and vector fields.
inline constexpr ThemeColor axisX = ThemeColor::hex(0xFF5A5A);
inline constexpr ThemeColor axisY = ThemeColor::hex(0x6CC23A);
inline constexpr ThemeColor axisZ = ThemeColor::hex(0x4F9BFF);
inline constexpr ThemeColor guideLine = ThemeColor::hex(0xE6FFF7, 0.85f);
inline constexpr ThemeColor activeObjectLabel = ThemeColor::hex(0xFFCF8A);

// Viewport scene overlays (editor/viewport/SceneGizmoDraw): 1px debug lines
// and name labels the renderer draws over the tone-mapped image, so these are
// display-referred like every other token. Selected and active objects take
// the selection hues whatever their kind.
inline constexpr ThemeColor overlayLight = ThemeColor::hex(0xFFE27A);
inline constexpr ThemeColor overlayEnvironment = ThemeColor::hex(0xB6FFE6);
inline constexpr ThemeColor overlayCamera = ThemeColor::hex(0xC8D6E0);
inline constexpr ThemeColor overlayActiveCamera = ThemeColor::hex(0x5EF0E0);
inline constexpr ThemeColor overlayCollider = ThemeColor::hex(0x6CFF8A);
inline constexpr ThemeColor overlayReflectionProbe = ThemeColor::hex(0x7AD8FF);
inline constexpr ThemeColor overlayDecal = ThemeColor::hex(0xC58AFF);
inline constexpr ThemeColor overlayIrradianceVolume = ThemeColor::hex(0x4F9BFF);
inline constexpr ThemeColor overlaySelected = ThemeColor::hex(0xFF9A3C);
inline constexpr ThemeColor overlayActive = ThemeColor::hex(0xFFE0A8);
inline constexpr ThemeColor overlayLabel = ThemeColor::hex(0xE6FFF7);

// Prefab instances: the bar beside an overridden value and its bold ink.
inline constexpr ThemeColor prefabOverride = ThemeColor::hex(0x4FA8FF);
inline constexpr ThemeColor prefabOverrideInk = ThemeColor::hex(0xCFE6FF);

// Pixel icon hues, chosen by meaning.
inline constexpr ThemeColor iconOutline = ThemeColor::hex(0x00060C);
inline constexpr ThemeColor iconGloss = ThemeColor::hex(0xFFFFFF, 0.55f);
inline constexpr ThemeColor iconBlue = ThemeColor::hex(0x2A86E0);
inline constexpr ThemeColor iconYellow = ThemeColor::hex(0xE6C21A);
inline constexpr ThemeColor iconOrange = ThemeColor::hex(0xE0801C);
inline constexpr ThemeColor iconGreen = ThemeColor::hex(0x46A81C);
inline constexpr ThemeColor iconTeal = ThemeColor::hex(0x16A6A0);
inline constexpr ThemeColor iconPurple = ThemeColor::hex(0x7050D0);
inline constexpr ThemeColor iconSlate = ThemeColor::hex(0x6D8296);
inline constexpr ThemeColor iconRed = ThemeColor::hex(0xE04848);
inline constexpr ThemeColor iconWhite = ThemeColor::hex(0xF4FFFB);

// The wash over the editor's glass, panes, and edge while a game runs
// (ThemeChrome::setModeTint): warm for Play, violet for Simulate, so the two
// are told apart at a glance. The play viewport's banner uses the Play hue.
inline constexpr ThemeColor playTint = ThemeColor::hex(0xFF7A2E, 0.2f);
inline constexpr ThemeColor simulateTint = ThemeColor::hex(0x8A5CFF, 0.2f);
inline constexpr ThemeColor playBanner = ThemeColor::hex(0x7A2A08, 0.9f);
inline constexpr ThemeColor playBannerInk = ThemeColor::hex(0xFFE6C8);

inline constexpr ThemeColor modalDim = ThemeColor::hex(0x000000, 0.5f);
// For backgrounds the theme paints itself.
inline constexpr ThemeColor transparent = ThemeColor::hex(0x000000, 0.0f);
} // namespace palette
