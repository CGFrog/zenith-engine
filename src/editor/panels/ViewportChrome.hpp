#pragma once

#include "editor/operators/Operator.hpp"

#include <cstdint>
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

    // The increment for the current gizmo mode; Select uses the scale value.
    float& activeSnap();
};

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
};

// Per-viewport presentation: which regions surround the 3D view (Blender's N
// sidebar and T tool shelf) and what the view draws.
struct ViewportChromeState {
    bool sidebarVisible{false};
    bool toolbarVisible{true};
    ViewportShading shading{ViewportShading::Rendered};
    ViewportOverlays overlays;
};

const char* viewportShadingLabel(ViewportShading shading);

// view3d.toggle_sidebar, view3d.toggle_toolbar, and view3d.shading_set. They
// edit state, which must outlive the registered operators; they need no
// window, so they also run headless.
std::vector<Operator> viewportChromeOperators(ViewportChromeState& state);

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
