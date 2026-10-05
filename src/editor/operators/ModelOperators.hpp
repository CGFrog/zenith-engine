#pragma once

#include "editor/operators/OperatorRegistry.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <string>

// Properties of object.model_replace.
struct ModelReplaceProperties {
    // The entity whose model is replaced (or which gets one).
    SceneEntityId entity;
    // The model file: project-relative (to the asset root) or absolute
    // inside it.
    std::string asset;
};

// Model operators, which need only the document and so run headless:
//   object.model_replace  gives an entity another model asset as one undo
//                         step: the previous model's node entities are
//                         destroyed, its Model and Animation components
//                         removed, and the new model instantiated under the
//                         entity, as a prefab re-expands a model. Children
//                         the user added under node entities move to the
//                         entity (keeping their world transforms), with a
//                         warning. Meshes are not swapped individually.
OperatorRegistrationResult registerModelOperators(OperatorRegistry& registry);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerModelOperatorFamily(OperatorRegistry& registry,
                                                       const EditorOperatorServices& services);
