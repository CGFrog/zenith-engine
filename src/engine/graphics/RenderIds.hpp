#pragma once

#include <cstdint>

// Identifiers shared by the render-state API (Rendering.hpp) and the picking
// types (RenderPicking.hpp), kept apart so both can name them.

// A scene entity's integral entt value as the renderer sees it (outlines).
using RenderObjectId = uint32_t;

// Generational handle of a render view (RenderRuntime::createView). A handle
// whose view was destroyed never matches a later view in the same slot.
struct RenderViewId {
    uint32_t index{0};
    uint32_t generation{0};

    explicit operator bool() const {
        return generation != 0;
    }

    bool operator==(const RenderViewId&) const = default;
};
