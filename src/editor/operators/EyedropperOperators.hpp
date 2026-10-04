#pragma once

#include "editor/operators/OperatorRegistry.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <string>

class EntityEyedropper;

// Properties of ui.eyedropper_entity: the entity reference field to fill.
struct EyedropperArmProperties {
    // The entity owning the component.
    SceneEntityId entity;
    // Component type key, for example "follow".
    std::string component;
    // Field keys from the component down, joined with '.', for example
    // "target" or "settings.target".
    std::string field;
};

// Properties of ui.eyedropper_pick.
struct EyedropperPickProperties {
    // The entity to store in the armed field.
    SceneEntityId entity;
};

// The entity eyedropper operators, acting on eyedropper (which must outlive
// the registry's use of them). They need only the document, so they run
// headless:
//   ui.eyedropper_entity  arms a pick for one entity reference field;
//   ui.eyedropper_pick    stores an entity in the armed field as one undo
//                         step, leaving the selection alone, and disarms;
//   ui.eyedropper_cancel  disarms without changing anything.
OperatorRegistrationResult registerEyedropperOperators(OperatorRegistry& registry, EntityEyedropper& eyedropper);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerEyedropperOperatorFamily(OperatorRegistry& registry,
                                                            const EditorOperatorServices& services);
