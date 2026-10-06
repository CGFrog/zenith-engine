#pragma once

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

// The editor's "8-bit Blender" theme: Blender's dark interface drawn as
// palette-limited pixel art, with square areas, hard one-art-pixel bevels, and
// a pixel font. Every colour the editor draws comes from here; panels and
// widgets never write colour literals.
namespace palette {
// The near-black gap between areas, also behind empty dock space.
inline constexpr ThemeColor areaGap = ThemeColor::hex(0x050505);
// Area headers (dock tab bars), the main menu bar, and the status bar.
inline constexpr ThemeColor header = ThemeColor::hex(0x1B1B1B);
inline constexpr ThemeColor headerHighlight = ThemeColor::hex(0x333333);
// The hard bevel raised controls, headers, and popups carry: highlight along
// the top and left edges, shade along the bottom and right.
inline constexpr ThemeColor bevelHighlight = ThemeColor::hex(0x8C8C8C);
inline constexpr ThemeColor bevelShade = ThemeColor::hex(0x000000);
// The one-art-pixel line around areas, controls, and popups.
inline constexpr ThemeColor outline = ThemeColor::hex(0x000000);
// Chrome floating over the 3D viewport: a translucent header.
inline constexpr ThemeColor overlayPanel = ThemeColor::hex(0x1B1B1B, 0.85f);
// Caption text. Over the 3D viewport it is outlined so it reads over any
// scene; on solid headers it is drawn plain.
inline constexpr ThemeColor captionInk = ThemeColor::hex(0xF0F0F0);
inline constexpr ThemeColor captionOutline = ThemeColor::hex(0x000000);

// Area contents.
inline constexpr ThemeColor pane = ThemeColor::hex(0x161616);
inline constexpr ThemeColor paneAlternate = ThemeColor::hex(0x1C1C1C);
inline constexpr ThemeColor popup = ThemeColor::hex(0x1F1F1F, 0.98f);
inline constexpr ThemeColor ink = ThemeColor::hex(0xF0F0F0);
inline constexpr ThemeColor inkMuted = ThemeColor::hex(0x9A9A9A);
inline constexpr ThemeColor inkDisabled = ThemeColor::hex(0x6E6E6E);
inline constexpr ThemeColor separator = ThemeColor::hex(0x2E2E2E);

// Raised buttons: flat grey with the bevel. A pressed button sinks; an active
// one (the current tool or tab) is lit in Blender's selection blue.
inline constexpr ThemeColor button = ThemeColor::hex(0x3C3C3C);
inline constexpr ThemeColor buttonHovered = ThemeColor::hex(0x4C4C4C);
inline constexpr ThemeColor buttonPressed = ThemeColor::hex(0x2A2A2A);
inline constexpr ThemeColor buttonActive = ThemeColor::hex(0x4772B3);
inline constexpr ThemeColor buttonActiveHighlight = ThemeColor::hex(0xB8D4FA);
inline constexpr ThemeColor buttonActiveShade = ThemeColor::hex(0x1A3560);
inline constexpr ThemeColor activeInk = ThemeColor::hex(0xFFFFFF);

// Recessed fields.
inline constexpr ThemeColor field = ThemeColor::hex(0x0A0A0A);
inline constexpr ThemeColor fieldHovered = ThemeColor::hex(0x121212);
inline constexpr ThemeColor fieldActive = ThemeColor::hex(0x1A1A1A);
inline constexpr ThemeColor textSelection = ThemeColor::hex(0x2C4A78);

// Collapsible section headers. Titles share the body font and stand out by
// colour.
inline constexpr ThemeColor section = ThemeColor::hex(0x2A2A2A);
inline constexpr ThemeColor sectionHovered = ThemeColor::hex(0x343434);
inline constexpr ThemeColor sectionInk = ThemeColor::hex(0xFFD070);

// List rows.
inline constexpr ThemeColor rowSelected = ThemeColor::hex(0x233A5E);
inline constexpr ThemeColor rowSelectedHighlight = ThemeColor::hex(0x4772B3);
inline constexpr ThemeColor rowSelectedShade = ThemeColor::hex(0x14243C);
inline constexpr ThemeColor rowHovered = ThemeColor::hex(0x222222);
inline constexpr ThemeColor indentGuide = ThemeColor::hex(0x3A3A3A);

// Sliders, checkboxes and scrollbars.
inline constexpr ThemeColor sliderFill = ThemeColor::hex(0x4772B3);
inline constexpr ThemeColor sliderHandle = ThemeColor::hex(0xDCDCDC);
inline constexpr ThemeColor scrollbarThumb = ThemeColor::hex(0x3C3C3C);
inline constexpr ThemeColor scrollbarThumbHovered = ThemeColor::hex(0x4C4C4C);

// The default accent (ThemeAccent::Blue): focus rings, active tab overlines,
// drag targets, and the gizmo selection.
inline constexpr ThemeColor accentBlue = ThemeColor::hex(0x5A8EE0);

// Message severities, as text and icon colours.
inline constexpr ThemeColor success = ThemeColor::hex(0x7CD860);
inline constexpr ThemeColor info = ThemeColor::hex(0x6AB0FF);
inline constexpr ThemeColor warning = ThemeColor::hex(0xFFC857);
inline constexpr ThemeColor error = ThemeColor::hex(0xFF8C8C);

// World axes, shared by the gizmo, modal transform guides and vector fields.
inline constexpr ThemeColor axisX = ThemeColor::hex(0xFF5A5A);
inline constexpr ThemeColor axisY = ThemeColor::hex(0x6CC23A);
inline constexpr ThemeColor axisZ = ThemeColor::hex(0x4F9BFF);
inline constexpr ThemeColor guideLine = ThemeColor::hex(0xF0F0F0, 0.85f);
inline constexpr ThemeColor activeObjectLabel = ThemeColor::hex(0xFFD070);

// Viewport scene overlays (editor/viewport/SceneGizmoDraw): 1px debug lines
// and name labels the renderer draws over the tone-mapped image, so these are
// display-referred like every other token. Selected and active objects take
// Blender's selection orange whatever their kind.
inline constexpr ThemeColor overlayLight = ThemeColor::hex(0xFFE27A);
inline constexpr ThemeColor overlayEnvironment = ThemeColor::hex(0xD8D8D8);
inline constexpr ThemeColor overlayCamera = ThemeColor::hex(0xC8C8C8);
inline constexpr ThemeColor overlayActiveCamera = ThemeColor::hex(0x6AB0FF);
inline constexpr ThemeColor overlayCollider = ThemeColor::hex(0x6CFF8A);
inline constexpr ThemeColor overlayReflectionProbe = ThemeColor::hex(0x7AD8FF);
inline constexpr ThemeColor overlayDecal = ThemeColor::hex(0xC58AFF);
inline constexpr ThemeColor overlayIrradianceVolume = ThemeColor::hex(0x4F9BFF);
inline constexpr ThemeColor overlaySelected = ThemeColor::hex(0xFF9A1F);
inline constexpr ThemeColor overlayActive = ThemeColor::hex(0xFFD070);
inline constexpr ThemeColor overlayLabel = ThemeColor::hex(0xF0F0F0);
// Alpha factor of an unselected collider or directional light, which draw
// faintly so objects without a surface are never visible only when selected.
inline constexpr float overlayIdleAlpha = 0.4f;
// The 3D cursor (editor/viewport/Cursor3D): a ring of alternating dashes with
// dark crosshair ticks, as in Blender, so it reads over light and dark scenes.
inline constexpr ThemeColor cursorRing = ThemeColor::hex(0xFF5A5A);
inline constexpr ThemeColor cursorRingAlternate = ThemeColor::hex(0xF4F4F4);
inline constexpr ThemeColor cursorCrosshair = ThemeColor::hex(0x000000);
// The point a modal move snapped to (surface or vertex snapping).
inline constexpr ThemeColor snapIndicator = ThemeColor::hex(0xFFE27A);
// Play-mode debugging overlays (editor/viewport/RuntimeDebugDraw): contacts,
// character capsules (grounded or airborne), and navigation agents (walking,
// on a partial path, or failed).
inline constexpr ThemeColor overlayContact = ThemeColor::hex(0xFF5AC8);
inline constexpr ThemeColor overlayCharacterGrounded = ThemeColor::hex(0x7CD860);
inline constexpr ThemeColor overlayCharacterAirborne = ThemeColor::hex(0x6AB0FF);
inline constexpr ThemeColor overlayNavAgent = ThemeColor::hex(0x7AD8FF);
inline constexpr ThemeColor overlayNavPartial = ThemeColor::hex(0xFFC857);
inline constexpr ThemeColor overlayNavFailed = ThemeColor::hex(0xFF5A5A);

// Prefab instances: the bar beside an overridden value and its bold ink.
inline constexpr ThemeColor prefabOverride = ThemeColor::hex(0x4FA8FF);
inline constexpr ThemeColor prefabOverrideInk = ThemeColor::hex(0xCFE6FF);

// Pixel icon hues, chosen by meaning.
inline constexpr ThemeColor iconOutline = ThemeColor::hex(0x000000);
inline constexpr ThemeColor iconGloss = ThemeColor::hex(0xFFFFFF, 0.55f);
inline constexpr ThemeColor iconBlue = ThemeColor::hex(0x2A86E0);
inline constexpr ThemeColor iconYellow = ThemeColor::hex(0xE6C21A);
inline constexpr ThemeColor iconOrange = ThemeColor::hex(0xE0801C);
inline constexpr ThemeColor iconGreen = ThemeColor::hex(0x46A81C);
inline constexpr ThemeColor iconTeal = ThemeColor::hex(0x16A6A0);
inline constexpr ThemeColor iconPurple = ThemeColor::hex(0x7050D0);
inline constexpr ThemeColor iconSlate = ThemeColor::hex(0x8A8A8A);
inline constexpr ThemeColor iconRed = ThemeColor::hex(0xE04848);
inline constexpr ThemeColor iconWhite = ThemeColor::hex(0xF4F4F4);

// The wash over the editor's headers, panes, and edge while a game runs
// (ThemeChrome::setModeTint): warm for Play, violet for Simulate, so the two
// are told apart at a glance. The play viewport's banner uses the Play hue.
inline constexpr ThemeColor playTint = ThemeColor::hex(0xFF7A2E, 0.2f);
inline constexpr ThemeColor simulateTint = ThemeColor::hex(0x8A5CFF, 0.2f);
// Prefab mode (a prefab open as its own document) washes in the prefab blue.
inline constexpr ThemeColor prefabTint = prefabOverride.withAlpha(0.2f);
inline constexpr ThemeColor playBanner = ThemeColor::hex(0x7A2A08, 0.9f);
inline constexpr ThemeColor playBannerInk = ThemeColor::hex(0xFFE6C8);

inline constexpr ThemeColor modalDim = ThemeColor::hex(0x000000, 0.5f);
// For backgrounds the theme paints itself.
inline constexpr ThemeColor transparent = ThemeColor::hex(0x000000, 0.0f);
} // namespace palette
