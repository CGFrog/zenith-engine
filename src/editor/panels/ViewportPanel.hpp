#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "editor/panels/ViewportChrome.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/vec2.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// A sidebar tab contributed from outside the viewport, for example by a game
// module, drawn after the built-in Item, Tool, and View tabs. Whoever adds it
// must remove it before the code draw runs is unloaded.
struct ViewportSidebarTab {
    // Tab label; unique among the viewport's tabs.
    std::string title;
    std::function<void(EditorContext&)> draw;
};

struct ViewportSidebarTabResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The 3D viewport: shows the renderer's offscreen image, drives the editor
// camera, and hosts Blender-style chrome over the image (a header with
// Shading, Overlays, and Gizmos menus, the T tool shelf on the left, and the N
// sidebar on the right) together with the gizmo, picking, and drop target.
// Clicks on the chrome never reach picking, the gizmo, or the camera.
class ViewportPanel final : public EditorPanel {
  public:
    const char* title() const override;
    int windowFlags() const override;
    void update(EditorContext& context, const InputState& input, float deltaSeconds) override;
    void draw(EditorContext& context) override;
    void hidden(EditorContext& context) override;

    TransformToolSettings& tools();
    const TransformToolSettings& tools() const;
    ViewportChromeState& chrome();
    const ViewportChromeState& chrome() const;
    // view3d.toggle_sidebar, view3d.toggle_toolbar, and view3d.shading_set
    // acting on this panel; the panel must outlive their registration.
    std::vector<Operator> chromeOperators();
    // Rejects an empty title, a missing draw function, and a title already
    // used by a built-in or added tab.
    ViewportSidebarTabResult addSidebarTab(ViewportSidebarTab tab);
    // Returns whether a tab with title was removed.
    bool removeSidebarTab(std::string_view title);

    // Screen-space rectangle of the viewport image from the latest draw.
    glm::vec2 position() const;
    glm::vec2 size() const;
    // A right-click that cancels a modal transform must not also start fly
    // navigation, so camera input waits until the button is released.
    void suppressCameraUntilRightRelease();

  private:
    void updateSelectionOutline(EditorContext& context);
    void drawViewImage(UiTextureHandle texture) const;
    // Each returns whether its region is under the mouse.
    bool drawChrome(EditorContext& context);
    bool drawHeader(EditorContext& context);
    bool drawToolShelf(float headerHeight);
    bool drawSidebar(EditorContext& context, float headerHeight);
    void drawItemTab(EditorContext& context);
    // Returns whether the gizmo is in use or under the mouse.
    bool drawGizmo(EditorContext& context);
    void pickAtMouse(EditorContext& context);
    void drawActiveName(EditorContext& context) const;
    void setInputActive(EditorContext& context, bool active);

    TransformToolSettings m_tools;
    ViewportChromeState m_chrome;
    std::vector<ViewportSidebarTab> m_sidebarTabs;
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
