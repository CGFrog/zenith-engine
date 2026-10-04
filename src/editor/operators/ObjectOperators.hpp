#pragma once

#include "editor/operators/OperatorRegistry.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

// Object-mode operators that need only the document and selection, so they
// run headless: adding (also an empty with one component), creating,
// duplicating, deleting, renaming, selecting, clearing transforms, adding
// components (to the active object) and removing them (from every selected
// object), and baking irradiance volumes. The component value operators are in
// ComponentOperators. Session operators that need a window (modal transforms, popups,
// files, Play mode, the camera) are registered by the editor session.

struct AddObjectProperties {
    // World-space position of the new object, in meters. An invocation
    // starts it at the scene's 3D cursor (SceneEditorData).
    glm::vec3 location{0.0f};
};

struct AddCubeProperties {
    // As AddObjectProperties::location: starts at the 3D cursor.
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
    // World-space position of the new object, in meters; starts at the 3D
    // cursor.
    glm::vec3 location{0.0f};
};

// Also registers the collection and visibility operators
// (CollectionOperators.hpp, VisibilityOperators.hpp).
OperatorRegistrationResult registerObjectOperators(OperatorRegistry& registry);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerObjectOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services);
