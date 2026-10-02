#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "engine/scene/Scene.hpp"

#include <functional>
#include <string>
#include <unordered_map>

class ComponentType;

// Shows and edits the active entity's name, transform, and components, and
// offers whole-selection actions when several entities are selected.
//
// Components are listed from the document's component registry. Each is drawn
// from its reflected fields unless a custom editor is registered for its key,
// so new component types appear here without inspector code.
class InspectorPanel final : public EditorPanel {
  public:
    // Draws a component in place and returns whether it changed.
    using CustomEditor = std::function<bool(EditorContext& context, Entity entity, void* component)>;

    InspectorPanel();

    const char* title() const override;
    bool editModeOnly() const override;
    void draw(EditorContext& context) override;

  private:
    void drawMultipleSelection(EditorContext& context);
    void drawName(EditorContext& context, Entity entity, SceneEntityId entityId);
    void drawTransform(EditorContext& context, SceneEntityId entityId);
    void drawComponents(EditorContext& context, SceneEntityId entityId);
    void drawComponent(EditorContext& context, const ComponentType& type, SceneEntityId entityId);
    void drawPreservedComponents(EditorContext& context, SceneEntityId entityId);
    void drawAssetWarnings(EditorContext& context, Entity entity);
    void drawAddComponentMenu(EditorContext& context, SceneEntityId entityId);
    // Bakes the volume's probes next to the project's other derived assets
    // and records the new bake reference as an undoable edit.
    void bakeIrradianceVolume(EditorContext& context, SceneEntityId volume);

    std::string m_filter;
    std::unordered_map<std::string, CustomEditor> m_customEditors;
};
