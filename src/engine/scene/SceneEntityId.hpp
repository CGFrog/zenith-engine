#pragma once

#include <compare>
#include <cstdint>

// Stable identity of a scene entity across saves, undo, and reloads; zero is
// "no entity". Kept apart from Scene.hpp so lightweight headers (render
// picking results) can name it without the ECS.
struct SceneEntityId {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    auto operator<=>(const SceneEntityId&) const = default;
};
