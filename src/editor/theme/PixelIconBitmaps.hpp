#pragma once

#include "editor/theme/EditorPalette.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

// The editor's pixel-art icons, drawn by PixelIcons. Kept free of ImGui so
// the bitmaps can be validated without a UI context.
enum class EditorIcon : uint8_t {
    // Viewport transform tools.
    ToolTweak,
    ToolMove,
    ToolRotate,
    ToolScale,
    // Entity kinds, picked from components by entityIcon.
    Camera,
    Sun,
    PointLight,
    SpotLight,
    Environment,
    Fog,
    ReflectionProbe,
    IrradianceVolume,
    Decal,
    Particles,
    AudioEmitter,
    AudioListener,
    Physics,
    Mesh,
    Empty,
    Scene,
    // Asset kinds.
    Folder,
    Texture,
    AudioClip,
    Script,
    // Outliner groups: a collection and a prefab asset.
    Collection,
    Prefab,
    // Properties tabs and panels.
    Components,
    Outliner,
    Properties,
    Viewport,
    Statistics,
    Build,
    // Message severities.
    Error,
    Warning,
    Info,
    Success,
    // Playback and dismissal.
    Play,
    Pause,
    Stop,
    Close,
};

inline constexpr size_t editorIconCount = static_cast<size_t>(EditorIcon::Close) + 1;
inline constexpr size_t iconBitmapSize = 10;

// A square palette-indexed bitmap. Each character is one art pixel:
//   '.' transparent      'o' outline
//   '1' body (hue)       '2' body shade       '3' body highlight
//   'w' white detail
struct IconBitmap {
    ThemeColor hue;
    std::array<std::string_view, iconBitmapSize> rows;
};

const IconBitmap& iconBitmap(EditorIcon icon);
// The colour a bitmap character stands for, or nullopt for '.' and unknown
// characters.
std::optional<ThemeColor> iconPixelColor(const IconBitmap& bitmap, char pixel);
