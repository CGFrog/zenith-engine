#pragma once

// The editor's object selection.

#include "engine/scene/Scene.hpp"

#include <cstddef>
#include <optional>
#include <span>
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
    // Shift click in a list (the outliner): selects the rows from anchor to
    // clicked in displayed order (see displayedRange), keeping anchor active
    // so further range clicks grow or shrink from the same row. Replace
    // drops everything outside the range; Extend (Ctrl Shift) adds the range
    // to the selection.
    void clickRange(std::span<const SceneEntityId> displayed, SceneEntityId anchor, SceneEntityId clicked,
                    SelectionClickMode mode);
    // Selects every entity the editor may select (SceneVisibility::selectable:
    // not hidden, selectable, renderable), keeping the current active entity
    // active.
    void selectAll(const Scene& scene);
    // Selects the selectable entities that are not selected.
    void invert(const Scene& scene);
    // Drops ids whose entities no longer exist, for example after undo.
    void removeMissing(const Scene& scene);

    // Selected entities whose ancestors are not also selected, so a parent and
    // child moved together are not transformed twice.
    std::vector<SceneEntityId> transformRoots(const Scene& scene) const;

  private:
    std::vector<SceneEntityId> m_ids;
};

// The entities of displayed (rows in the order a list draws them, which may
// list an entity more than once) from anchor to clicked, both included, in
// displayed order and without repeats. Uses the first row of each entity.
// Just clicked when anchor is not displayed; empty when clicked is not.
std::vector<SceneEntityId> displayedRange(std::span<const SceneEntityId> displayed, SceneEntityId anchor,
                                          SceneEntityId clicked);
