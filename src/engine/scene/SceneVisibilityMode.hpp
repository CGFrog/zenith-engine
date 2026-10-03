#pragma once

#include <cstdint>

// Which visibility flags apply when deciding what to draw (see
// SceneVisibility.hpp). Kept apart so render settings can name a mode
// without including the scene.
enum class SceneVisibilityMode : uint8_t {
    // Games, Play mode, and bakes: only `renderable` flags apply, so the
    // editor's viewport hide never changes what a player sees.
    Runtime,
    // Editor viewports: `renderable` plus the editor hide (entity `hidden`,
    // collection `visible`).
    Editor,
};
