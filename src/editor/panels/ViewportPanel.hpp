#pragma once

#include "editor/application/EditorPlayMode.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/panels/EditorPanel.hpp"
#include "editor/panels/ViewportChrome.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/vec2.hpp>

#include <optional>
#include <string>
#include <vector>

// The renderer's shading mode for a viewport shading choice.
ViewShading toViewShading(ViewportShading shading);

// The per-view settings override a viewport hands its render view: none
// (follow the project's settings) outside Edit mode, so Play mode always shows
// the game's look, and when the project's settings already shade that way;
// otherwise a copy of projectSettings with the viewport's shading, reduced
// for that mode (viewShadingFrameSettings, including the capability
// fallback). The project's settings themselves are never changed.
std::optional<RenderSettings> viewportViewSettings(const RenderSettings& projectSettings,
                                                   const RenderCapabilities& capabilities, ViewportShading shading,
                                                   EditorMode mode);

// One 3D viewport area: shows its own render view, drives its own editor
// camera, and hosts Blender-style chrome over the image (a header with View,
// Shading, Overlays, and Gizmos menus, the T tool shelf on the left, and the N
// sidebar on the right) together with the gizmo, picking, and drop target.
// Clicks on the chrome never reach picking, the gizmo, or the camera.
//
// Any number of viewports may be open. Each registers with the session's
// ViewportRegistry for its lifetime, takes a render view from it the first
// time it is drawn, and returns the view when destroyed. Hovering or clicking
// a viewport makes it the active one that view3d operators act on.
//
// A viewport can look through a scene camera instead of its editor camera
// (Blender's camera view): navigation is then disabled and every view
// operator or the View menu returns to the editor camera.
class ViewportPanel final : public EditorPanel {
  public:
    explicit ViewportPanel(ViewportRegistry& registry);
    ~ViewportPanel() override;
    ViewportPanel(const ViewportPanel&) = delete;
    ViewportPanel& operator=(const ViewportPanel&) = delete;

    const char* title() const override;
    int windowFlags() const override;
    void update(EditorContext& context, const InputState& input, float deltaSeconds) override;
    void draw(EditorContext& context) override;
    void hidden(EditorContext& context) override;

    ViewportId id() const;
    // Empty until the viewport is first drawn, or while the renderer has no
    // free view.
    RenderViewId renderView() const;
    // Returns the render view to the registry; the next draw takes one again.
    void releaseRenderView();
    EditorCamera& camera();
    const EditorCamera& camera() const;
    TransformToolSettings& tools();
    const TransformToolSettings& tools() const;
    ViewportChromeState& chrome();
    const ViewportChromeState& chrome() const;

    // Looks through entity's camera; fails (returning false) when it has
    // none.
    bool lookThroughCamera(const Scene& scene, SceneEntityId entity);
    void exitCameraView();
    std::optional<SceneEntityId> cameraView() const;
    // The lens the viewport shows: the scene camera it looks through while
    // that camera exists, otherwise its editor camera.
    RenderView lens(const Scene& scene) const;
    // The settings override sent to the render view in the latest draw.
    const std::optional<RenderSettings>& appliedViewSettings() const;

    // Screen-space rectangle of the viewport image from the latest draw.
    glm::vec2 position() const;
    glm::vec2 size() const;
    // A right-click that cancels a modal transform must not also start fly
    // navigation, so camera input waits until the button is released.
    void suppressCameraUntilRightRelease();

  private:
    // Pushes the camera, shading, grid, and outline to the render view.
    void syncRenderView(EditorContext& context);
    void updateSelectionOutline(EditorContext& context);
    void drawViewImage(UiTextureHandle texture, const char* placeholder) const;
    // Each returns whether its region is under the mouse.
    bool drawChrome(EditorContext& context);
    bool drawHeader(EditorContext& context);
    void drawViewMenu(EditorContext& context);
    bool drawToolShelf(float headerHeight);
    bool drawSidebar(EditorContext& context, float headerHeight);
    void drawItemTab(EditorContext& context);
    // Returns whether the gizmo is in use or under the mouse.
    bool drawGizmo(EditorContext& context);
    void pickAtMouse(EditorContext& context);
    void drawActiveName(EditorContext& context) const;
    void setInputActive(EditorContext& context, bool active);

    ViewportRegistry& m_registry;
    ViewportId m_id;
    // The runtime m_view came from, kept to return the view on destruction.
    RenderRuntime* m_rendering{nullptr};
    RenderViewId m_view;
    EditorCamera m_camera;
    std::optional<SceneEntityId> m_cameraView;
    std::optional<RenderSettings> m_appliedViewSettings;
    TransformToolSettings m_tools;
    ViewportChromeState m_chrome;
    bool m_inputActive{false};
    bool m_navigating{false};
    bool m_suppressCameraUntilRelease{false};
    glm::vec2 m_position{0.0f};
    glm::vec2 m_size{1.0f};
    // Where the chrome leaves the image uncovered, for the name label.
    glm::vec2 m_labelOrigin{0.0f};
    bool m_gizmoWasUsing{false};
    // Scene snapshot from before the current gizmo drag, for undo.
    std::string m_gizmoBefore;
    std::vector<RenderObjectId> m_outlinedObjects;
    std::vector<Entity> m_outlineTraversal;
};
