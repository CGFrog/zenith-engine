#pragma once

#include "editor/operators/OperatorRegistry.hpp"
#include "engine/scene/Collections.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

// Headless collection operators (Blender-style collections, see
// engine/scene/Collections.hpp). All are undoable: the collection definitions
// and the membership components change in one scene edit. Collections are
// addressed by their stable id; ids above 2^32 - 1 cannot be named here.
//
//   collection.new             create a collection, optionally linking the selection
//   collection.delete          delete a collection (contents move to its parent)
//   collection.rename          rename a collection
//   collection.link            add the selected objects to a collection
//   collection.unlink          remove the selected objects from a collection
//   collection.set_flag        set a collection's visible, selectable, or renderable flag
//   object.collection_instance_add   add an empty that instances a collection

struct NewCollectionProperties {
    std::string name{"Collection"};
    // Enclosing collection id; 0 creates a top-level collection.
    uint32_t parent{0};
    // Links the selected objects to the new collection.
    bool linkSelection{true};
    // Written back with the id of the collection created.
    uint32_t created{0};
};

struct CollectionProperties {
    uint32_t collection{0};
};

struct RenameCollectionProperties {
    uint32_t collection{0};
    // Leading and trailing whitespace is dropped; the rest must not be
    // empty. Names need not be unique (collections are named by id).
    std::string name;
};

struct CollectionFlagProperties {
    uint32_t collection{0};
    CollectionFlag flag{CollectionFlag::Visible};
    bool value{true};
};

struct CollectionInstanceProperties {
    uint32_t collection{0};
    // World-space position of the new instance, in meters.
    glm::vec3 location{0.0f};
};

OperatorRegistrationResult registerCollectionOperators(OperatorRegistry& registry);
