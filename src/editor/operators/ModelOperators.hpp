#pragma once

#include "editor/operators/OperatorRegistry.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Properties of object.model_replace.
struct ModelReplaceProperties {
    // The entity whose model is replaced (or which gets one).
    SceneEntityId entity;
    // The model file: project-relative (to the asset root) or absolute
    // inside it.
    std::string asset;
};

// One entity object.mesh_assign gives the mesh.
struct MeshAssignTarget {
    SceneEntityId entity;
};

// Properties of object.mesh_assign.
struct MeshAssignProperties {
    // The entities that get the mesh; invoking without them takes the
    // selection.
    std::vector<MeshAssignTarget> entities;
    // The model file holding the mesh, as for object.model_replace.
    std::string asset;
    // The mesh's glTF index in the model (AssetManager::loadMesh).
    uint32_t mesh{0};
    // Also point a triangle-mesh collider on the same entity that used the
    // entity's previous mesh at the new one. Off by default: collision shapes
    // are often deliberately simpler than what is drawn.
    bool updateCollider{false};
};

// Model operators, which need only the document and so run headless:
//   object.model_replace  gives an entity another model asset as one undo
//                         step: the previous model's node entities are
//                         destroyed, its Model and Animation components
//                         removed, and the new model instantiated under the
//                         entity, as a prefab re-expands a model. Children
//                         the user added under node entities move to the
//                         entity (keeping their world transforms), with a
//                         warning.
//   object.mesh_assign    gives entities one mesh of a model as one undo
//                         step, adding a Mesh Renderer to those without one
//                         (see MeshAssignProperties and the rules below).
//
// How object.mesh_assign treats an entity that belonged to a model instance
// (a model node), whose node index would no longer describe what it draws:
//   - The entity leaves the instance: its Model Node component is removed,
//     so the model's animation no longer moves it and a model replace treats
//     it as a user object. An Info diagnostic says how many left.
//   - Its Skin stays only when the model it skins with draws the new mesh
//     with that same skin on some node (another mesh of the same skinned
//     character); otherwise the Skin is removed, so the renderer never pairs
//     the new mesh with the old joint palette.
//   - Its Morph Weights stay when the new mesh has that many morph targets,
//     are reset to the model's default weights for the mesh (zeros when no
//     node lists them) when the count differs, and are removed when the new
//     mesh has none.
// A node that is a joint of its model's skeleton is refused: removing its
// Model Node would break the skin that follows it. The skin and morph rules
// apply to any entity, model node or not.
OperatorRegistrationResult registerModelOperators(OperatorRegistry& registry);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerModelOperatorFamily(OperatorRegistry& registry,
                                                       const EditorOperatorServices& services);
