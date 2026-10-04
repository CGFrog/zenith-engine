#pragma once

#include "editor/document/PrefabAuthoring.hpp"
#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ComponentInspectorLayout;
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

// Draws and edits entity properties: name, transform, components, and the
// actions on them. The Inspector and Properties panels both present the
// selection through it, so they share widgets, filtering, and undo.
//
// Every edit is recorded through the context's SceneEditing as one undoable
// command whose coalesce key names the entities and what changed, so a drag
// merges into one step. filter is a case-insensitive label filter (see
// matchesInspectorFilter); empty shows everything.
//
// Components are listed from the document's component registry, in the
// display order of the ComponentInspectorLayout, which also remembers which
// sections are collapsed. Each is drawn from its reflected fields unless a
// custom editor is registered for its key, so new component types appear
// without view code.
//
// Several entities are edited together: a component they all have is shown
// with the first (active) entity's values, a field whose values differ shows
// mixedValueText, and an edit changes that field on every entity in one undo
// step. Holding Alt while editing also applies the edited field to every
// other selected entity with the component, for example from a section only
// the active entity has. The name and transform are edited together the same
// way.
//
// A component header offers Remove and, on right click or its "..." button, a
// menu running the component operators (reset, copy and paste values, copy
// to selected, move). Those act on the selection, so only views of the
// selection offer them, and a component only the active entity has offers
// only the actions that read the active entity (see HeaderActions).
class EntityPropertiesView {
  public:
    // Draws a component in place and returns whether it changed.
    using CustomEditor = std::function<bool(EditorContext& context, Entity entity, void* component)>;

    // What a component header offers.
    enum class HeaderActions : uint8_t {
        // Nothing: the component is not the selection's (the World tab).
        None,
        // Copy Values, Copy to Selected, and moving, for a component only the
        // active entity of the selection has.
        ActiveEntity,
        // Remove and the whole menu; they act on every selected entity with
        // the component.
        Selection,
    };

    struct ComponentDrawOptions {
        std::string_view filter;
        HeaderActions actions{HeaderActions::Selection};
        // The category label the view lists, so a section moves among the
        // visible sections; empty when every category is listed.
        std::string_view category;
    };

    // The layout must outlive the view.
    explicit EntityPropertiesView(ComponentInspectorLayout& layout);

    // The selection count and whole-selection actions.
    void drawMultipleSelection(EditorContext& context) const;
    // The name field (one "Rename Entity" step per entity while typing) and id.
    void drawName(EditorContext& context, SceneEntityId entityId) const;
    // The name field of several entities: their name, or mixedValueText when
    // the names differ. Typing gives every entity the typed name, as one
    // undo step while typing. One entity also shows its id and prefab
    // override, as above.
    void drawName(EditorContext& context, std::span<const SceneEntityId> entityIds) const;
    // A full-width label filter input editing filter.
    void drawFilter(const char* id, std::string& filter) const;
    void drawTransform(EditorContext& context, SceneEntityId entityId, std::string_view filter) const;
    // The transform section of several entities: Position, Rotation, and
    // Scale of the first, with mixedValueText for an axis whose values
    // differ. An edit changes only the edited axis on every entity, in one
    // undo step. Prefab overrides are marked only for a single entity.
    void drawTransform(EditorContext& context, std::span<const SceneEntityId> entityIds, std::string_view filter) const;
    // The visible components of entityIds (the active entity first), or only
    // those whose category label is category: first those every entity has,
    // then, under a heading, those only the first entity has.
    void drawComponents(EditorContext& context, std::span<const SceneEntityId> entityIds, std::string_view filter,
                        std::optional<std::string_view> category = std::nullopt) const;
    // One component of entityIds, showing the first entity's values; entities
    // without the component are skipped.
    void drawComponent(EditorContext& context, const ComponentType& type, std::span<const SceneEntityId> entityIds,
                       const ComponentDrawOptions& options) const;
    // Data kept for component types no registry knows (for example from an
    // unloaded game module).
    void drawPreservedComponents(EditorContext& context, SceneEntityId entityId) const;
    // Bakes the entity's irradiance volume when it has one.
    void drawIrradianceBakeButton(EditorContext& context, SceneEntityId entityId) const;
    void drawAssetWarnings(EditorContext& context, SceneEntityId entityId) const;
    // A drop target placing a dragged model asset as a child of the entity.
    void drawModelDropTarget(EditorContext& context, SceneEntityId entityId) const;
    // For an entity of a prefab instance: the prefab asset's name, the
    // instance's override summary, and Apply, Revert, Unpack, and Select
    // Prefab Asset (the prefab operators, acting on the active entity's
    // instance). Draws nothing for other entities.
    void drawPrefabHeader(EditorContext& context, SceneEntityId entityId) const;

    // How the entity differs from its prefab, as the view highlights it;
    // cached until the document revision or the prefab file changes.
    const PrefabEntityOverrides& prefabOverrides(const EditorContext& context, SceneEntityId entityId) const;

  private:
    // An operator the component header requested, run after the component is
    // drawn so it cannot invalidate the component being edited.
    struct HeaderAction {
        std::string operatorId;
        std::string properties;
    };

    std::optional<HeaderAction> drawComponentMenu(EditorContext& context, const ComponentType& type,
                                                  const ComponentDrawOptions& options,
                                                  std::optional<SceneEntityId> overriddenEntity) const;
    // Draws the values of the first target, copying edits to the others, and
    // records a change as one undo step. Returns a field's "Revert to Prefab"
    // request, run after drawing.
    std::optional<HeaderAction> drawComponentValues(EditorContext& context, const ComponentType& type,
                                                    std::span<const Entity> shown, std::string_view filter) const;

    ComponentInspectorLayout& m_layout;
    std::unordered_map<std::string, CustomEditor> m_customEditors;
    // Override highlighting reads the prefab; kept between frames.
    mutable PrefabOverrideCache m_prefabOverrides;
};
