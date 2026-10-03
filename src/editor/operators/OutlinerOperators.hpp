#pragma once

#include "editor/operators/OperatorRegistry.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstdint>

// Headless operators behind the outliner's own gestures. Both are undoable.
//
//   object.set_visibility   set one entity's visible, selectable, or renderable
//                           flag (the outliner's restriction columns)
//   outliner.parent_drop    parent the selected objects to an entity, or to
//                           the scene root (dropping rows on a row)

// The VisibilityComponent flag an outliner column edits. Visible is the
// inverse of VisibilityComponent::hidden, the editor-only viewport hide.
enum class EntityVisibilityFlag : uint8_t {
    Visible,
    Selectable,
    Renderable,
};

struct EntityVisibilityProperties {
    SceneEntityId entity;
    EntityVisibilityFlag flag{EntityVisibilityFlag::Visible};
    bool value{true};
};

struct ParentDropProperties {
    // The new parent; none parents to the scene root.
    SceneEntityId parent;
    // Keeps the objects where they are in the world; otherwise their local
    // transforms are kept and they move with the new parent.
    bool keepTransform{true};
};

OperatorRegistrationResult registerOutlinerOperators(OperatorRegistry& registry);
