#pragma once

#include "editor/areas/WorkspaceFile.hpp"

#include <cstdint>
#include <span>
#include <string_view>

struct EditorPanelVisibility;

// Where a default workspace docks an area. The default layout splits the
// dockspace into a left column, a right column (with a lower part), and a
// bottom row around the central node; slots without areas are not split off.
enum class DefaultDockSlot {
    Center,
    Left,
    Right,
    RightBottom,
    Bottom,
    // Not docked.
    Floating,
};

// One area of a default workspace. A type listed n times gets areas numbered
// 1 to n in list order (only multiple-instance types may repeat).
struct DefaultArea {
    std::string_view type;
    DefaultDockSlot slot;
    // The panel's initial per-area state (EditorPanel::restoreAreaState),
    // for example the Properties tab; empty for the panel's defaults.
    std::string_view state{};
};

// A workspace the editor creates for new users and when a workspace is reset.
// Types it does not list get a hidden area.
struct DefaultWorkspace {
    std::string_view name;
    std::span<const DefaultArea> areas;
};

// Layout, Lighting, and Debug, in tab order. Changing them requires bumping
// currentWorkspaceFileVersion so saved workspaces pick up the new defaults.
std::span<const DefaultWorkspace> defaultWorkspaces();
// The default named name, or the first default for a workspace the editor
// does not define.
const DefaultWorkspace& defaultWorkspaceFor(std::string_view name);
// The default area that places area number of typeId (the number-th listed
// area of the type, or the first when there are fewer); null when the
// workspace does not list the type.
const DefaultArea* defaultAreaFor(const DefaultWorkspace& workspace, std::string_view typeId, uint32_t number);
WorkspaceRecord defaultWorkspaceRecord(const DefaultWorkspace& workspace);
WorkspaceFileContents defaultWorkspaceFile();

// What editors before workspaces.json stored: panel.* flags and
// layout.version in settings.ini, and the ImGui layout in layout.ini.
struct LegacyEditorLayout {
    const EditorPanelVisibility& panels;
    uint32_t layoutVersion{0};
    // layout.ini text, empty when the file does not exist.
    std::string_view imguiLayout;
};

// The default workspaces, with the Layout workspace taking the legacy panel
// visibility and docking layout (window names rewritten from "<title>" to
// "<title>##<type>.1"). A layout older than the last legacy version, which
// lacked panels, is dropped in favour of the default one, as before.
WorkspaceFileContents migrateLegacyLayout(const LegacyEditorLayout& legacy);
