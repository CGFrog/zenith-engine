#pragma once

#include "editor/operators/OperatorRegistry.hpp"

#include <glm/vec3.hpp>

#include <filesystem>
#include <functional>
#include <string>

// Headless prefab operators (PrefabAuthoring workflows) acting on the active
// entity's prefab instance:
//
//   prefab.create_from_selection {path}   new prefab from the active entity
//   prefab.instantiate {asset, parent, location}  new instance, selected
//   prefab.apply                          write the instance into its prefab
//   prefab.revert                         reset the instance to its prefab
//   prefab.unpack                         keep the entities, drop the link
//   prefab.revert_field {entity, component, field}
//   prefab.select_asset                   show the prefab in the Asset Browser
//
// Undo: every operator is one undo step covering its scene changes (links,
// instance entities, other instances an apply refreshed). Prefab files are
// written immediately and stay as written when the step is undone or redone:
// undoing create_from_selection unlinks the entity but keeps the new file, and
// undoing apply restores the scene's instances while the file keeps the
// applied content (they then show the difference as overrides). Asset files
// are user content managed by the project, like a bake output.

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
};

OperatorRegistrationResult registerPrefabOperators(OperatorRegistry& registry, PrefabOperatorServices services = {});

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerPrefabOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services);
