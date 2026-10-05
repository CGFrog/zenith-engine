#pragma once

#include "editor/operators/OperatorRegistry.hpp"

#include <cstdint>

// Headless selection operators (Blender's Select menu). They change only the
// selection, so they record no undo step, and pick only entities the editor
// may select (SceneVisibility::selectable in Editor mode):
//
//   object.select_hierarchy {direction, extend}   (] and [)
//       children: the selected objects' direct children; parent: their
//       parents. The active object's first child (or its parent) becomes
//       active.
//   object.select_grouped {type, extend}          (Shift+G)
//       everything sharing a property with the active object (see
//       SelectGroupedType); the active object stays active.
//
// Without extend the result replaces the selection; with it the result is
// added. Finding nothing cancels and leaves the selection as it was.

enum class SelectHierarchyDirection : uint8_t {
    Children,
    Parent,
};

struct SelectHierarchyProperties {
    SelectHierarchyDirection direction{SelectHierarchyDirection::Children};
    bool extend{false};
};

enum class SelectGroupedType : uint8_t {
    // Every descendant of the selected objects.
    Children,
    // The selected objects' direct children.
    ImmediateChildren,
    // Instance roots of the active object's prefab.
    Prefab,
    // Roots instancing the active object's model (or, for a model node,
    // its instance root's model).
    Model,
    // Objects drawing the active object's mesh.
    Mesh,
    // Objects sharing a tag with the active object.
    Tag,
    // Objects sharing a collection with the active object.
    Collection,
    // Objects having every component type the active object has.
    Components,
};

struct SelectGroupedProperties {
    SelectGroupedType type{SelectGroupedType::Children};
    bool extend{false};
};

OperatorRegistrationResult registerSelectOperators(OperatorRegistry& registry);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerSelectOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services);
