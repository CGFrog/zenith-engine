#pragma once

#include "editor/operators/OperatorRegistry.hpp"

#include <string>

// Headless prefab operators (PrefabAuthoring workflows) acting on the active
// entity's prefab instance:
//
//   prefab.create_from_selection {path}   new prefab from the active entity
//   prefab.instantiate {asset}            new instance at the scene root
//   prefab.apply                          write the instance into its prefab
//   prefab.revert                         reset the instance to its prefab
//   prefab.unpack                         keep the entities, drop the link
//   prefab.revert_field {entity, component, field}
//
// Undo: every operator is one undo step covering its scene changes (links,
// instance entities, other instances an apply refreshed). Prefab files are
// written immediately and stay as written when the step is undone or redone:
// undoing create_from_selection unlinks the entity but keeps the new file, and
// undoing apply restores the scene's instances while the file keeps the
// applied content (they then show the difference as overrides). Asset files
// are user content managed by the project, like a bake output.

struct CreatePrefabProperties {
    // Project-relative path of the new prefab, ending in .prefab.
    std::string path;
};

struct InstantiatePrefabProperties {
    // Project-relative path of the prefab asset.
    std::string asset;
};

struct RevertPrefabFieldProperties {
    // Stable id of the instance entity, in decimal; empty uses the active entity.
    std::string entity;
    // Component type key; empty for the entity's name or local transform.
    std::string component;
    // JSON pointer into the component's document object (empty for the whole
    // component), or "/name" or "/local_transform" without a component.
    std::string field;
};

OperatorRegistrationResult registerPrefabOperators(OperatorRegistry& registry);
