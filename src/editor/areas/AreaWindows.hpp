#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

class AreaRegistry;
class ThemeChrome;
class Workspace;
class WorkspaceSet;
struct AreaInstance;
struct AreaType;
struct DefaultWorkspace;

// What the user asked for in an area's header or window frame. The caller
// applies it after drawing every area (by running the matching wm.area_*
// operator), because it may add or remove areas.
struct AreaWindowRequest {
    enum class Kind {
        None,
        Close,
        ChangeType,
        NewArea,
    };

    Kind kind{Kind::None};
    // The type to show for ChangeType and NewArea.
    std::string typeId;
};

struct AreaWindowResult {
    AreaWindowRequest request;
    // The window's dock node after drawing (0 when floating), for
    // AreaInstance::dockId.
    uint32_t dockId{0};
};

// The editor type button in an area's header: its menu label (the type's
// title, then room for the arrow; the id after ### stays the same across
// types) and the down arrow drawn at the right end of the button's rectangle
// (screen pixels), sized to the font and centred vertically.
std::string editorTypeMenuLabel(std::string_view title);

struct EditorTypeArrow {
    float x{0.0f};
    float y{0.0f};
    float size{0.0f};
};

EditorTypeArrow editorTypeArrow(float itemMinY, float itemMaxX, float itemMaxY, float fontSize, float artPixel);

// Draws a visible area as an ImGui window named area.windowName(type.title):
// moves it into its requested dock node, paints its themed frame with
// chrome, draws the header (a menu bar whose editor-type dropdown lists every
// registered type), then drawContents while the window is open. drawContents may run operators that remove the area,
// so the area is not touched after it; the caller stores the dock node.
AreaWindowResult drawAreaWindow(AreaInstance& area, const AreaType& type, const AreaRegistry& registry, int windowFlags,
                                const ThemeChrome& chrome, const std::function<void()>& drawContents);

// The ImGui settings (windows and docking) as text, for Workspace::layout.
std::string captureImGuiLayout();
// Replaces the ImGui settings with layout (cleared when empty). Call between
// frames: after ImGui::Render and before ImGui::NewFrame.
void restoreImGuiLayout(std::string_view layout);
// Applies a requested workspace switch: captures the active workspace's
// layout, activates the requested one, and restores its layout. Call between
// frames. Returns whether the workspace changed.
bool applyRequestedWorkspace(WorkspaceSet& workspaces);

// Docks the workspace's visible areas into dockspaceId as the default
// describes; other areas are left floating. Call inside a frame, after the
// dockspace is submitted and before the areas are drawn.
void buildDefaultDockLayout(uint32_t dockspaceId, const DefaultWorkspace& defaults, const Workspace& workspace,
                            const AreaRegistry& registry);
