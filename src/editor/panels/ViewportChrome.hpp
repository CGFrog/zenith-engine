#pragma once

#include "editor/interaction/ModalTransform.hpp"
#include "editor/operators/Operator.hpp"
#include "editor/viewport/SceneGizmoDraw.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class EditorCamera;

enum class GizmoMode {
    Select,
    Translate,
    Rotate,
    Scale,
};

enum class TransformSpace {
    Global,
    Local,
};

// Transform tool choices shared by the viewport gizmo and modal G/R/S.
struct TransformToolSettings {
    // Blender starts with the selection tool, which shows no gizmo.
    GizmoMode gizmo{GizmoMode::Select};
    TransformSpace space{TransformSpace::Global};
    bool snapping{false};
    float translationSnap{1.0f};
    float rotationSnapDegrees{5.0f};
    float scaleSnap{0.1f};
    // What translation snaps to (ModalTransform.hpp); rotation and scale
    // always snap in increments.
    SnapTarget snapTarget{SnapTarget::Increment};
    // Increment snapping onto the absolute grid rather than in steps from
    // where the selection started.
    bool absoluteGridSnap{false};
    // What rotation and scale turn about.
    TransformPivot pivot{TransformPivot::MedianPoint};

    // The increment for the current gizmo mode; Select uses the scale value.
    float& activeSnap();
};

// The snapping a transform evaluates with: the tool's choices, with
// `enabled` inverted while invert (Ctrl) is held. targetPoint stays empty for
// the caller to fill.
TransformSnapping transformSnapping(const TransformToolSettings& tools, bool invert);

// Labels of the pivot and snap target choices, as menus show them.
const char* transformPivotLabel(TransformPivot pivot);
const char* snapTargetLabel(SnapTarget target);

// The tool settings an operator acts on when it runs (the active viewport's),
// or null when there is none.
using TransformToolTarget = std::function<TransformToolSettings*()>;

// transform.pivot_set {pivot} (median_point, active_element,
// individual_origins, cursor) and transform.snap_target_set {target}
// (increment, surface, vertex), acting on target's settings. They poll false
// while target returns null and run headless.
std::vector<Operator> transformToolOperators(TransformToolTarget target);

// How one viewport shades the scene, mirroring Blender's header buttons. The
// values match the engine's view shading modes one to one; ViewportPanel maps
// them to the renderer in a single place.
enum class ViewportShading : uint8_t {
    Rendered,
    MaterialPreview,
    Solid,
    Wireframe,
};

// Editor overlays drawn over (or into) one viewport's image.
struct ViewportOverlays {
    bool grid{true};
    bool selectionOutline{true};
    // The active object's name in the top-left corner.
    bool activeName{true};
    bool gizmo{true};
    // The 3D cursor (editor/viewport/Cursor3D).
    bool cursor{true};
    // Scene gizmos recorded into the viewport's render view (lights, cameras,
    // colliders, volumes, selection bounds, names).
    SceneGizmoOverlays scene;
    // Whether the scene gizmos also draw in Play mode. Even then a viewport
    // looking through a scene camera shows the game's image untouched.
    bool sceneInPlay{false};
};

// One overlay of ViewportOverlays, as named by the overlay operators and the
// saved area state.
enum class ViewportOverlay : uint8_t {
    Grid,
    SelectionOutline,
    ActiveName,
    TransformGizmo,
    Lights,
    Cameras,
    Colliders,
    ReflectionProbes,
    Decals,
    IrradianceVolumes,
    Names,
    SelectionBounds,
    SceneInPlay,
    Cursor3D,
};

// The overlay's on/off flag, or null for a scoped (Off/Selected/All) overlay.
bool* viewportOverlayFlag(ViewportOverlays& overlays, ViewportOverlay overlay);
// The overlay's scope, or null for an on/off overlay.
OverlayScope* viewportOverlayScope(ViewportOverlays& overlays, ViewportOverlay overlay);

// Per-viewport presentation: which regions surround the 3D view (Blender's N
// sidebar and T tool shelf) and what the view draws.
struct ViewportChromeState {
    bool sidebarVisible{false};
    bool toolbarVisible{true};
    ViewportShading shading{ViewportShading::Rendered};
    ViewportOverlays overlays;
};

const char* viewportShadingLabel(ViewportShading shading);

// The chrome an operator acts on when it runs (the active viewport's), or null
// when there is none.
using ViewportChromeTarget = std::function<ViewportChromeState*()>;

// view3d.toggle_sidebar, view3d.toggle_toolbar, view3d.shading_set,
// view3d.overlay_toggle {overlay} (an on/off overlay flips; a scoped one
// switches between Off and its default scope), and view3d.overlay_scope_set
// {overlay, scope} acting on target's chrome. They poll false, and fail if
// run anyway, while target returns null. They need no window, so they also
// run headless.
std::vector<Operator> viewportChromeOperators(ViewportChromeTarget target);

// The viewport's per-area state saved in workspaces.json: a JSON object whose
// "overlays" member holds every overlay by its operator name. Restoring
// ignores text it does not understand and keeps the defaults of missing
// overlays, so an empty state restores the defaults.
std::string viewportAreaState(const ViewportChromeState& chrome);
void restoreViewportAreaState(std::string_view state, ViewportChromeState& chrome);
// The same object with the transform tool choices (pivot, snapping, and the
// snap increments) under a "tools" member, restored with the same tolerance.
std::string viewportAreaState(const ViewportChromeState& chrome, const TransformToolSettings& tools);
void restoreViewportAreaState(std::string_view state, ViewportChromeState& chrome, TransformToolSettings& tools);

// Widgets for the viewport's header, tool shelf, and sidebar. Each draws into
// the current ImGui window and edits the referenced settings directly.
namespace ViewportChromeWidgets {
// Header menu bodies, drawn between BeginMenu and EndMenu.
void drawShadingMenu(ViewportChromeState& state);
void drawOverlaysMenu(ViewportOverlays& overlays);
void drawGizmosMenu(TransformToolSettings& tools);
// The vertical tool shelf: Tweak, Move, Rotate, Scale.
void drawToolShelf(TransformToolSettings& tools, float buttonWidth);
// Sidebar tab bodies.
void drawToolTab(TransformToolSettings& tools);
void drawViewTab(EditorCamera& camera);
} // namespace ViewportChromeWidgets
