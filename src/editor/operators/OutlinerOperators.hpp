#pragma once

// Headless operators behind the outliner's own gestures.

#include "editor/operators/OperatorRegistry.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstdint>
#include <string>

// Headless operators behind the outliner's own gestures. All but
// outliner.select_matching are undoable.
//
//   object.set_visibility    set one entity's visible, selectable, or renderable
//                            flag (the outliner's restriction columns)
//   outliner.parent_drop     parent the selected objects to an entity, or to
//                            the scene root (dropping rows on a row)
//   outliner.reorder         move one object to a position among a parent's
//                            children (dropping a row between two rows)
//   outliner.select_matching select every object an outliner search matches

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

struct ReorderProperties {
    SceneEntityId entity;
    // The parent the object is placed under; root objects have no stored
    // order, so it must name an object.
    SceneEntityId parent;
    // The object's position among the parent's children afterwards (0 is
    // first), clamped to the last position.
    uint32_t index{0};
    // When the parent changes: keeps the object where it is in the world.
    bool keepTransform{true};
};

struct SelectMatchingProperties {
    // An outliner search (panels/outliner/OutlinerFilter.hpp syntax).
    std::string filter;
};

OperatorRegistrationResult registerOutlinerOperators(OperatorRegistry& registry);
