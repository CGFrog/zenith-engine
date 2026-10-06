#pragma once

// What Apply to Edit Scene copies from the runtime scene of Play or Simulate back into the edit scene.

#include "engine/scene/SceneDocument.hpp"

#include <span>
#include <string>

class AssetManager;

// What Apply to Edit Scene copies from the runtime scene of Play or Simulate
// back into the edit scene.
struct RuntimeApplyRequest {
    // The entities to apply, by stable id.
    std::span<const SceneEntityId> entities;
    // A component type key: only that component's runtime value is applied
    // (added to the edit entity when it lacks it). Empty applies the whole
    // entity: its name, local transform, and every component, so components
    // the runtime entity no longer has are removed. The edit entity keeps its
    // parent either way; an entity that exists only at runtime is created
    // under its runtime parent when the edit scene has that parent (or it is
    // applied too), and at the root otherwise. Whenever that parent is not
    // the running one, the local transform is converted so the entity keeps
    // its running world placement.
    std::string component;
};

// Builds the change that makes the edit scene's requested entities match the
// runtime scene, comparing their scene-document forms. Each change's before
// side is the edit scene's current entity, so the result can be recorded as
// an EntityDiffCommand on the edit history (SceneEditing::applyToEditScene).
// Entities that already match are left out, so an empty result means there is
// nothing to apply. Neither scene is changed.
//
// Fails, describing why, when a requested entity is not in the runtime
// scene, when applying a component to an entity the edit scene does not have,
// or when the runtime entity has no such component.
SceneDocumentResult runtimeChangesForEditScene(const Scene& runtimeScene, const Scene& editScene,
                                               const AssetManager& assets, const SceneDocumentOptions& options,
                                               const RuntimeApplyRequest& request, SceneChanges& changes);
