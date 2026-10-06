#pragma once

// Copies entity subtrees as a scene document fragment and pastes them as new entities with fresh ids.

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
// Both cost what the fragment holds, not the scene: copy serializes only the
// copied subtrees (and names the collections they reference, which paste
// matches by name), and paste creates the copies in the scene with fresh ids
// (SceneDocument::createEntities), so every other entity keeps its EnTT
// handle, components, and runtime state (physics bodies in Play).
//
// Transient entities (Scene::isTransient) are never copied: a copied root
// that is transient is skipped, as is a transient child. Paste pastes at the
// top level when parent is transient, since copies under it would be
// transient too.
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
    // One fragment holding rootWorlds.size() copies of fragment, so one
    // paste creates them all. Copy c gives
    // root r the world transform rootWorlds[c][r] (each element lists one
    // matrix per root, in roots order) when pasted at the top level, and
    // keeps its local transform under a parent; entity references inside a copy
    // follow to that copy. Pasting returns the roots copy by copy.
    static SceneDocumentResult replicate(std::string_view fragment, const SceneDocumentOptions& options,
                                         std::span<const std::vector<glm::mat4>> rootWorlds, std::string& output);
};
