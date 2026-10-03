#pragma once

#include "editor/operators/OperatorRegistry.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

// Object-mode operators that need only the document and selection, so they
// run headless: adding (also an empty with one component), creating, duplicating, deleting, renaming, selecting,
// clearing transforms, adding/removing components, and baking irradiance
// volumes. Session operators that need a window (modal transforms, popups,
// files, Play mode, the camera) are registered by the editor session.

struct AddObjectProperties {
    // World-space position of the new object, in meters.
    glm::vec3 location{0.0f};
};

struct AddCubeProperties {
    glm::vec3 location{0.0f};
    // Uniform scale; the default cube is one meter across.
    float size{1.0f};
};

enum class NewEntityPlacement : uint8_t {
    Root,
    // Child of the active entity when exactly one entity is selected,
    // otherwise at the scene root.
    Child,
    // Beside the active entity, under the same parent.
    Sibling,
};

struct CreateEntityProperties {
    NewEntityPlacement placement{NewEntityPlacement::Child};
};

struct DuplicateProperties {
    // Added to each copy's local location, in its parent's space.
    glm::vec3 offset{0.0f};
};

enum class SelectAllAction : uint8_t {
    Select,
    Deselect,
    Invert,
};

struct SelectAllProperties {
    SelectAllAction action{SelectAllAction::Select};
};

struct RenameProperties {
    std::string name;
};

struct ComponentProperties {
    // Component type key, for example "point_light".
    std::string component;
};

struct AddEmptyWithProperties {
    // Component type key, for example "point_light".
    std::string component;
    // World-space position of the new object, in meters.
    glm::vec3 location{0.0f};
};

OperatorRegistrationResult registerObjectOperators(OperatorRegistry& registry);
