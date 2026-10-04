#pragma once

#include "editor/application/EditorPlayMode.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/panels/EditorPanel.hpp"
#include "editor/panels/ViewportChrome.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/viewport/BoxSelect.hpp"
#include "editor/viewport/LassoSelect.hpp"
#include "editor/viewport/ViewportPicker.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/vec2.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The renderer's shading mode for a viewport shading choice.
ViewShading toViewShading(ViewportShading shading);

// Whose shading a viewport shows.
enum class ViewportShadingSource : uint8_t {
    // The viewport's own shading choice (Solid, Wireframe, ...).
    Viewport,
    // The project's render settings: the game's look.
    Project,
};

// Play (running or paused) shows the game's look; Edit and Simulate, which
// like Blender's simulation is an editor view of the running scene, keep each
// viewport's own shading. simulating is EditorPlayMode::simulating.
ViewportShadingSource viewportShadingSource(EditorMode mode, bool simulating);

// The per-view settings override a viewport hands its render view: none
// (follow the project's settings) when the source is Project, so Play mode
// always shows the game's look, and when the project's settings already shade
// that way; otherwise a copy of projectSettings with the viewport's shading,
// reduced for that mode (viewShadingFrameSettings, including the capability
// fallback). The project's settings themselves are never changed.
std::optional<RenderSettings> viewportViewSettings(const RenderSettings& projectSettings,
                                                   const RenderCapabilities& capabilities, ViewportShading shading,
                                                   ViewportShadingSource source);

// Whether a viewport records its scene gizmos (ViewportOverlays::scene): always
// in Edit mode; in Play mode only when the user enabled sceneInPlay, and never
// while the viewport looks through a scene camera, so the game's own image
// stays untouched.
bool viewportDrawsSceneGizmos(EditorMode mode, bool lookingThroughSceneCamera, const ViewportOverlays& overlays);

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
//
// Selection: a left click runs view3d.select (Alt: view3d.select_cycle,
// Shift extends), B starts the box tool (view3d.select_box) and Ctrl+RMB
// drags a lasso (view3d.select_lasso). Picks resolve through the GPU id
// buffer when the view renders and fall back to CPU ray casts otherwise
// (ViewportPicker documents the timing); an armed eyedropper takes a click
// instead of the selection. Local view (view3d.localview) isolates some
// entities in this viewport only: its render view draws and picks just them.
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
    // The overlays (viewportAreaState).
    std::string areaState() const override;
    void restoreAreaState(std::string_view state) override;

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

    // Queues a click selection at a view-pixel position (continuous, origin
    // at the image's top-left corner). The next draw starts its pick and
    // applies it when it resolves: to the armed eyedropper, else to the
    // selection (a miss clears it unless extending). cycle steps through
    // the entities under the point, front to back, from the active one.
    void requestClickSelect(glm::vec2 viewPoint, bool extend, bool cycle);
    // The mouse position over the image in the latest draw, in view pixels.
    glm::vec2 mouseInView() const;
    // Modal box and lasso tools; at most one runs at a time, and starting
    // one cancels the other.
    void beginBoxSelect(SelectionSetMode mode);
    void beginLassoSelect(SelectionSetMode mode, bool rightDrag);
    void cancelSelectTool();
    bool selectToolActive() const;
    const BoxSelect& boxSelect() const;
    const LassoSelect& lassoSelect() const;
    // Whether a pick waits for the GPU.
    bool pickPending() const;
    // The latest resolved pick; empty before the first.
    const std::optional<ViewportPickOutcome>& lastPickOutcome() const;

    // Local view: isolates the existing entities among `entities` and their
    // descendants, remembering the camera, and frames them. Returns false
    // (changing nothing) when none exists or local view is already on.
    bool enterLocalView(Scene& scene, const AssetManager& assets, std::span<const SceneEntityId> entities);
    // Restores the camera (and camera view) from before local view.
    void exitLocalView();
    bool localViewActive() const;
    // The entities local view was entered with; empty outside local view.
    std::span<const SceneEntityId> localViewEntities() const;
    // The sorted render objects isolated in the latest draw (the entities and
    // their descendants that still exist); nullopt outside local view.
    const std::optional<std::vector<RenderObjectId>>& localViewObjects() const;

  private:
    // A click or region pick waiting for the next draw to start it.
    struct QueuedSelect {
        ViewportPickKind kind{ViewportPickKind::Click};
        glm::vec2 point{0.0f};
        RegionSelection region;
        bool extend{false};
    };

    // What a started pick will do once it resolves.
    struct SelectIntent {
        ViewportPickKind kind{ViewportPickKind::Click};
        bool extend{false};
        SelectionSetMode mode{SelectionSetMode::Set};
        // The eyedropper was armed when the click started.
        bool eyedropper{false};
    };

    struct LocalView {
        std::vector<SceneEntityId> entities;
        EditorCamera previousCamera;
        std::optional<SceneEntityId> previousCameraView;
    };

    // Pushes the camera, shading, grid, outline, and scene gizmos to the
    // render view.
    void syncRenderView(EditorContext& context);
    void updateSelectionOutline(EditorContext& context);
    void recordSceneGizmos(EditorContext& context);
    // The name labels the render view projected in its latest frame.
    void drawSceneLabels(EditorContext& context) const;
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
    // Runs view3d.select or view3d.select_cycle for a click on the image.
    void selectAtMouse(EditorContext& context);
    // Starts the Ctrl+RMB lasso through view3d.select_lasso.
    void startLassoGesture(EditorContext& context);
    void updateSelectTools(bool imageHovered);
    // Starts a queued pick and applies a resolved one.
    void processPicks(EditorContext& context);
    void applyPick(EditorContext& context, const SelectIntent& intent, const ViewportPickOutcome& outcome);
    // Recomputes the isolated objects and hands them to the render view.
    void syncLocalView(EditorContext& context);
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
    glm::vec2 m_mouseInView{0.0f};
    BoxSelect m_boxSelect;
    LassoSelect m_lassoSelect;
    std::optional<QueuedSelect> m_queuedSelect;
    ViewportPicker m_picker;
    SelectIntent m_pendingIntent;
    std::optional<ViewportPickOutcome> m_lastPickOutcome;
    std::optional<LocalView> m_localView;
    std::optional<std::vector<RenderObjectId>> m_localViewObjects;
    // The isolation last handed to m_view; m_isolationView is empty until
    // the first hand-over and after the view is released.
    RenderViewId m_isolationView;
    std::optional<std::vector<RenderObjectId>> m_sentIsolation;
};
