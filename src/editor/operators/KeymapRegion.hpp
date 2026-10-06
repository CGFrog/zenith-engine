#pragma once

// The editor area a key binding belongs to.

#include <cstdint>

// The editor area a key binding belongs to. As in Blender, keys go to the
// region under the mouse; Global bindings work everywhere.
enum class KeymapRegion : uint8_t {
    Global,
    Viewport,
    Outliner,
};
