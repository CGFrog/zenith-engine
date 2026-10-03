#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <span>
#include <vector>

class AssetManager;

// Copies entity subtrees as a scene document fragment and pastes them as new
// entities with fresh ids. Entity references between pasted entities are
// remapped to the copies (see remapEntityReferences); references to entities
// outside the fragment keep naming the same entity.
class SceneFragment {
  public:
    static SceneDocumentResult copy(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options,
                                    std::span<const SceneEntityId> roots, std::string& fragment);
    static SceneDocumentResult paste(std::string_view fragment, Scene& scene, AssetManager& assets,
                                     const SceneDocumentOptions& options, Entity parent,
                                     std::vector<SceneEntityId>& pastedRoots);
};
