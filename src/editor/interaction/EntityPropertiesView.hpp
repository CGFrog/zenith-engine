#pragma once

#include "engine/scene/Scene.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ComponentType;
class ComponentTypeRegistry;
struct EditorContext;

// The category heading a component type is grouped under: its category, or
// "Other" when it has none.
std::string componentCategoryLabel(const ComponentType& type);

// Category labels of the non-internal components the entity has, in the order
// the registry first lists them. Empty when the entity does not exist.
std::vector<std::string> entityComponentCategories(const ComponentTypeRegistry& types, const Scene& scene,
                                                   SceneEntityId entityId);

// Draws and edits one entity's properties: name, transform, components, and
// the actions on them. The Inspector and Properties panels both present an
// entity through it, so they share widgets, filtering, and undo.
//
// Every edit is recorded through the context's SceneEditing as one undoable
// command whose coalesce key names the entity and what changed, so a drag
// merges into one step. Component add and remove run the object operators,
// which act on the active object. filter is a case-insensitive label filter
// (see matchesInspectorFilter); empty shows everything.
//
// Components are listed from the document's component registry. Each is drawn
// from its reflected fields unless a custom editor is registered for its key,
// so new component types appear without view code.
class EntityPropertiesView {
  public:
    // Draws a component in place and returns whether it changed.
    using CustomEditor = std::function<bool(EditorContext& context, Entity entity, void* component)>;

    struct ComponentDrawOptions {
        std::string_view filter;
        // Offer a Remove button in the header. It removes from the active
        // object, so only views of the active entity should offer it.
        bool removable{true};
    };

    EntityPropertiesView();

    // The selection count and whole-selection actions.
    void drawMultipleSelection(EditorContext& context) const;
    // The name field (one "Rename Entity" step per entity while typing) and id.
    void drawName(EditorContext& context, SceneEntityId entityId) const;
    // A full-width label filter input editing filter.
    void drawFilter(const char* id, std::string& filter) const;
    void drawTransform(EditorContext& context, SceneEntityId entityId, std::string_view filter) const;
    // Every visible component the entity has, or only those whose category
    // label is category.
    void drawComponents(EditorContext& context, SceneEntityId entityId, std::string_view filter,
                        std::optional<std::string_view> category = std::nullopt) const;
    void drawComponent(EditorContext& context, const ComponentType& type, SceneEntityId entityId,
                       const ComponentDrawOptions& options) const;
    // Data kept for component types no registry knows (for example from an
    // unloaded game module).
    void drawPreservedComponents(EditorContext& context, SceneEntityId entityId) const;
    // Bakes the entity's irradiance volume when it has one.
    void drawIrradianceBakeButton(EditorContext& context, SceneEntityId entityId) const;
    void drawAssetWarnings(EditorContext& context, SceneEntityId entityId) const;
    // A drop target placing a dragged model asset as a child of the entity.
    void drawModelDropTarget(EditorContext& context, SceneEntityId entityId) const;
    // An Add Component button and popup grouped by category, or listing only
    // one category's types.
    void drawAddComponentMenu(EditorContext& context, SceneEntityId entityId,
                              std::optional<std::string_view> category = std::nullopt) const;

  private:
    std::unordered_map<std::string, CustomEditor> m_customEditors;
};
