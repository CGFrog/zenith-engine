#pragma once

#include "engine/scene/Scene.hpp"

#include <cstddef>
#include <optional>
#include <vector>

enum class SelectionClickMode {
    // The clicked entity becomes the only selected entity.
    Replace,
    // Shift/Ctrl click: adds the entity as the new active one, makes an
    // already selected entity active, or deselects the active entity.
    Extend,
};

// The editor's object selection. As in Blender, the first entry is the active
// entity that the inspector and local transform axes follow. Entities are held
// by stable SceneEntityId so the selection survives undo and document loads,
// which give recreated entities new EnTT handles.
class Selection {
  public:
    const std::vector<SceneEntityId>& ids() const;
    bool empty() const;
    size_t size() const;
    std::optional<SceneEntityId> active() const;
    bool contains(SceneEntityId id) const;

    void clear();
    void assign(std::vector<SceneEntityId> ids);
    void selectOnly(SceneEntityId id);
    void click(SceneEntityId id, SelectionClickMode mode);
    // Selects every entity, keeping the current active entity active.
    void selectAll(const Scene& scene);
    void invert(const Scene& scene);
    // Drops ids whose entities no longer exist, for example after undo.
    void removeMissing(const Scene& scene);

    // Selected entities whose ancestors are not also selected, so a parent and
    // child moved together are not transformed twice.
    std::vector<SceneEntityId> transformRoots(const Scene& scene) const;

  private:
    std::vector<SceneEntityId> m_ids;
};
