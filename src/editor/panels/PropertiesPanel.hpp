#pragma once

#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/panels/EditorPanel.hpp"

#include <string>
#include <vector>

class RenderSettingsController;

enum class PropertiesTabKind {
    // The active entity's name and transform, or the selection summary.
    Object,
    // The selection's components of one category of the active entity.
    Components,
    // The scene's primary sun, environment, and fog, and its irradiance volumes.
    World,
    // The session's render settings.
    Render,
    // Information about the open document.
    Scene
};

struct PropertiesTab {
    PropertiesTabKind kind{PropertiesTabKind::Object};
    // The component category label, for Components tabs only.
    std::string category;

    // The label shown in the tab strip.
    std::string label() const;

    bool operator==(const PropertiesTab&) const = default;
};

// A Blender-style Properties editor: a vertical tab strip beside the selected
// tab's contents. Entity tabs present the selection through the same
// EntityPropertiesView as the Inspector, so edits share its filtering,
// multi-entity editing, and undo through SceneEditing. The Render tab edits
// the session's RenderSettingsController, shared with the Graphics panel.
//
// Authoring tabs are read-only outside Edit mode; the Render and Scene tabs
// stay usable in Play mode.
class PropertiesPanel final : public EditorPanel {
  public:
    // The controller and layout must outlive the panel.
    PropertiesPanel(RenderSettingsController& renderSettings, ComponentInspectorLayout& layout);

    const char* title() const override;
    void draw(EditorContext& context) override;

    // The tabs available now, in strip order: Object, one Components tab per
    // category the active entity has, then World, Render, and Scene.
    std::vector<PropertiesTab> tabs(const EditorContext& context) const;
    // The remembered tab when it is available now, otherwise Object.
    PropertiesTab activeTab(const EditorContext& context) const;
    // Remembers tab; it is shown whenever it is available.
    void selectTab(PropertiesTab tab);

    // The remembered tab, so each Properties area keeps its own: "object",
    // "world", "render", "scene", or "components:<category>". Restoring
    // selects that tab; unknown text leaves the selection unchanged.
    std::string areaState() const override;
    void restoreAreaState(std::string_view state) override;

  private:
    void drawTabStrip(const std::vector<PropertiesTab>& tabs, const PropertiesTab& active);
    void drawObjectTab(EditorContext& context);
    void drawComponentsTab(EditorContext& context, const std::string& category);
    void drawWorldTab(EditorContext& context);
    void drawRenderTab(EditorContext& context);
    void drawSceneTab(EditorContext& context) const;
    // The watched fields (debug.watch_field) above every tab, with the
    // active scene's values: the running scene's in Play and Simulate.
    static void drawWatchedFields(EditorContext& context);

    RenderSettingsController& m_renderSettings;
    EntityPropertiesView m_view;
    PropertiesTab m_selected;
    std::string m_filter;
};
