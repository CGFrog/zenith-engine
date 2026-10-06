#pragma once

// Moves camera to show the entities' bounds (each mesh's bounds sphere, or a small sphere around an entity without a
// loaded mesh)

#include "editor/interaction/EditorCamera.hpp"

#include <span>

// Moves camera to show the entities' bounds (each mesh's bounds sphere, or a
// small sphere around an entity without a loaded mesh); entities that no
// longer exist are skipped and nothing moves when none remain. Updates the
// scene's world transforms first.
void frameEntities(EditorCamera& camera, Scene& scene, const AssetManager& assets,
                   std::span<const SceneEntityId> entities);
