#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <span>
#include <vector>

class AssetManager;

class SceneFragment {
  public:
    static SceneDocumentResult copy(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options,
                                    std::span<const SceneEntityId> roots, std::string& fragment);
    static SceneDocumentResult paste(std::string_view fragment, Scene& scene, AssetManager& assets,
                                     const SceneDocumentOptions& options, Entity parent,
                                     std::vector<SceneEntityId>& pastedRoots);
};
