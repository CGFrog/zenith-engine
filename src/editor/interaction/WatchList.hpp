#pragma once

#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;

// One component field the user watches: an entity by its stable id, a
// component key, and a JSON pointer into the component's document object
// ("/range", "/offset/1"), with the label it was watched under.
struct WatchedField {
    SceneEntityId entity;
    std::string component;
    std::string pointer;
    std::string label;

    // The same field, whatever its label.
    bool sameField(const WatchedField& other) const {
        return entity == other.entity && component == other.component && pointer == other.pointer;
    }
};

// The fields the Properties panel shows while the game runs (debug.watch_field
// and debug.unwatch_field). Editor state for this session: not part of the
// document or undo, and ids that stop existing are kept, so a watch survives
// Stop and the next Play.
class WatchList {
  public:
    // Adds field; false when it is already watched.
    bool add(WatchedField field);
    // Removes the field; false when it was not watched.
    bool remove(const WatchedField& field);
    void clear();
    std::span<const WatchedField> fields() const;
    bool contains(const WatchedField& field) const;

  private:
    std::vector<WatchedField> m_fields;
};

// The watched field's current value in scene, as compact JSON as the scene
// document writes it (runtime-state fields included), or why it has none:
// "(no entity)", "(no component)", or "(no field)".
std::string watchedValueText(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options,
                             const WatchedField& field);
