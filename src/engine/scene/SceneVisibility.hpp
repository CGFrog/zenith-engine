#pragma once

#include "Scene.hpp"
#include "SceneVisibilityMode.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

// Which entities a renderer draws and the editor may pick, from the entity
// VisibilityComponent and the flags of its collections.
//
// An entity outside every collection is governed by its own flags only. An
// entity in collections also needs at least one of them to allow the flag,
// where a collection allows it when it and every enclosing collection have it
// set (Blender's rule: an object linked to a visible collection stays visible
// even when another of its collections is hidden).
//
// Evaluates visibility for one scene state. It resolves every collection's
// effective flags once, so construct it per pass over the scene and drop it
// before the scene's collections change. The scene must outlive it.
class SceneVisibility {
  public:
    SceneVisibility(const Scene& scene, SceneVisibilityMode mode);

    SceneVisibilityMode mode() const {
        return m_mode;
    }

    // Whether a renderer in this mode draws the entity itself.
    bool drawn(Entity entity) const;
    // Whether the entity renders when another entity instances one of its
    // collections. Only `renderable` flags apply: hiding the source
    // collection in the viewport, the usual instancing workflow, keeps its
    // instances visible.
    bool drawnInInstance(Entity entity) const;
    // Whether the editor may pick the entity: drawn in this mode, and
    // selectable by its own flag and its collections'.
    bool selectable(Entity entity) const;

  private:
    bool collectionsAllow(Entity entity, CollectionFlag flag) const;

    const Scene& m_scene;
    SceneVisibilityMode m_mode;
    // Bit (1 << CollectionFlag) set when the collection allows the flag.
    std::unordered_map<uint64_t, uint8_t> m_allowed;
};

// One entity drawn again by a collection instance.
struct CollectionInstanceDraw {
    // The instanced entity, whose components (mesh, skin, ...) are drawn.
    Entity member{entt::null};
    // The outermost entity holding the CollectionInstanceComponent that
    // produced this draw; pickers, outlines, and per-view isolation treat the
    // draw as part of it.
    Entity instancer{entt::null};
    // Where the member is drawn: instancer world * translate(-instance offset)
    // * member world, composed through any nested instances.
    glm::mat4 world{1.0f};
};

// Nested instances (an instanced collection containing an instancer) expand
// to at most this depth; deeper levels and cycles are skipped.
constexpr uint32_t maximumCollectionInstanceDepth = 8;

// Lists every draw produced by collection instances in the scene, in a
// deterministic order (instancers by stable id, then members by stable id).
// An instancer contributes only when visibility draws it; its members are
// filtered by drawnInInstance. World transforms must be current.
void expandCollectionInstances(const Scene& scene, const SceneVisibility& visibility,
                               std::vector<CollectionInstanceDraw>& draws);
