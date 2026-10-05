#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <glm/mat4x4.hpp>

#include <span>
#include <vector>

class AssetManager;

// Copies entity subtrees as a scene document fragment and pastes them as new
// entities with fresh ids. Entity references between pasted entities are
// remapped to the copies (see remapEntityReferences); references to entities
// outside the fragment keep naming the same entity.
//
// A fragment's roots have no parent; each keeps its local transform and also
// carries its world transform, so a root pasted at the top level lands where
// its original was, and one pasted under a parent keeps its local transform
// (a duplicate beside its original lands on it).
//
// Transient entities (Scene::isTransient) are never copied: a copied root
// that is transient is skipped. Paste rebuilds the scene from its document,
// so it drops the scene's transient entities (a generator's preview), and it
// pastes at the top level when parent is transient.
class SceneFragment {
  public:
    static SceneDocumentResult copy(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options,
                                    std::span<const SceneEntityId> roots, std::string& fragment);
    static SceneDocumentResult paste(std::string_view fragment, Scene& scene, AssetManager& assets,
                                     const SceneDocumentOptions& options, Entity parent,
                                     std::vector<SceneEntityId>& pastedRoots);
    // The ids the fragment's roots had in the copied scene, in the order
    // paste returns their copies.
    static SceneDocumentResult roots(std::string_view fragment, std::vector<SceneEntityId>& sourceRoots);
    // One fragment holding rootWorlds.size() copies of fragment, so pasting
    // it rebuilds the scene once however many copies there are. Copy c gives
    // root r the world transform rootWorlds[c][r] (each element lists one
    // matrix per root, in roots order) when pasted at the top level, and
    // keeps its local transform under a parent; entity references inside a copy
    // follow to that copy. Pasting returns the roots copy by copy.
    static SceneDocumentResult replicate(std::string_view fragment, const SceneDocumentOptions& options,
                                         std::span<const std::vector<glm::mat4>> rootWorlds, std::string& output);
};
