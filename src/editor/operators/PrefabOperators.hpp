#pragma once

#include "editor/application/EditorSessionCommands.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
#include "editor/operators/OperatorRegistry.hpp"

#include <glm/vec3.hpp>

#include <filesystem>
#include <functional>
#include <string>

// Headless prefab operators (PrefabAuthoring workflows) acting on the active
// entity's prefab instance:
//
//   prefab.create_from_selection {path}   new prefab from the active entity
//   prefab.instantiate {asset, parent, location, rotation, normal, align_to_normal}
//                                         new instance, selected
//   prefab.apply                          write the instance into its prefab
//   prefab.revert                         reset the instance to its prefab
//   prefab.unpack                         keep the entities, drop the link
//   prefab.revert_field {entity, component, field}
//   prefab.select_asset                   show the prefab in the Asset Browser
//   prefab.edit {asset}                   open a prefab as its own document
//   prefab.exit_edit                      back to the document under it
//
// prefab.edit and prefab.exit_edit switch documents (prefab mode, see
// application/DocumentStack.hpp) and record nothing themselves: edits made in
// prefab mode go to the prefab's own history, and returning after saving
// records one "Edit <name>" step on the document below.
//
// Undo: every operator is one undo step covering its scene changes (links,
// instance entities, other instances an apply refreshed). create_from_selection
// and apply write their prefab file at once and record it in the same step (a
// PrefabWriteCommand the runner records from the result's undoStep): undoing
// apply puts back the file's previous content and the instances, undoing
// create removes the new file and unlinks the entity, and redo writes the
// file again. A file changed outside the editor since the step is never
// overwritten: that undo or redo fails with an error and changes nothing.

struct CreatePrefabProperties {
    // Project-relative path of the new prefab, ending in .prefab. Empty picks
    // a free "prefabs/<name>.prefab" (see PrefabAuthoring::suggestPath) and
    // writes it back, so F3 and the Object menu need no file dialog.
    std::string path;
};

struct InstantiatePrefabProperties {
    // Path of the prefab asset: project-relative, or absolute inside the
    // project asset root (as an Asset Browser drag carries it).
    std::string asset;
    // Stable id of the entity to parent the instance to, in decimal; empty
    // places it at the scene root.
    std::string parent;
    // World position of the instance root, in meters; an invocation starts
    // it at the 3D cursor (the root's transform places an instance and is
    // never an override).
    glm::vec3 location{0.0f};
    // Euler angles in degrees turning the instance root from the prefab's
    // own orientation, applied in the frame the alignment leaves
    // (placementRotation, PlacementOperators.hpp).
    glm::vec3 rotation{0.0f};
    // World surface normal at location, for alignToNormal; a viewport drop
    // passes the one under the mouse.
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    // Turns the instance's +Y onto normal.
    bool alignToNormal{false};
};

struct EditPrefabProperties {
    // Path of the prefab asset: project-relative, or absolute inside the
    // project asset root. Empty edits the prefab of the active entity's
    // instance.
    std::string asset;
};

struct RevertPrefabFieldProperties {
    // Stable id of the instance entity, in decimal; empty uses the active entity.
    std::string entity;
    // Component type key; empty for the entity's name, local transform, or
    // parent.
    std::string component;
    // JSON pointer into the component's document object (empty for the whole
    // component), or "/name", "/local_transform", or "/parent" without a
    // component.
    std::string field;
};

// What the prefab operators need from the running editor. Headless callers
// may leave a service unset; the operator using it then does not poll.
struct PrefabOperatorServices {
    // Shows an asset (an absolute path) in the primary Asset Browser, for
    // prefab.select_asset; the session routes it through ProjectAssets.
    std::function<void(const std::filesystem::path&)> revealAsset;
    // Told about every prefab file write (and its undo and redo), so the
    // session drops cached copies and notes the write in its ledger, and
    // about the files an instantiate read.
    PrefabWriteHooks writeHooks;
    // Prefab mode, for prefab.edit and prefab.exit_edit; null leaves them
    // unavailable.
    EditorPrefabModeCommands* prefabMode{nullptr};
};

OperatorRegistrationResult registerPrefabOperators(OperatorRegistry& registry, PrefabOperatorServices services = {});

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerPrefabOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services);
