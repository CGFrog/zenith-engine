#pragma once

#include "engine/reflection/Reflection.hpp"

#include <compare>
#include <cstdint>
#include <type_traits>

// The stable identity of a scene entity. It is saved in scene documents and
// survives save, load, undo, and redo, unlike EnTT handles. Zero means none.
struct SceneEntityId {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    auto operator<=>(const SceneEntityId&) const = default;
};

// A SceneEntityId member is reflected as an entity reference. Reflection
// reads and writes the id through its only member, so the layout is fixed.
template <> struct ReflectedFieldKind<SceneEntityId> {
    static constexpr FieldKind kind = FieldKind::EntityReference;
};

static_assert(std::is_standard_layout_v<SceneEntityId> && sizeof(SceneEntityId) == sizeof(uint64_t),
              "Entity reference fields store the id as a single uint64_t");
