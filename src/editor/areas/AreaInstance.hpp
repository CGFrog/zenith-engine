#pragma once

// One area of a workspace: a dockable window showing an editor type.

#include "editor/panels/EditorPanel.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// One area of a workspace: a dockable window showing an editor type.
//
// Its id is "<type>.<number>" (unique per type within the workspace) and its
// ImGui window is named "<title>##<id>", so each area keeps its own dock slot
// in the workspace layout.
struct AreaInstance {
    std::string typeId;
    uint32_t number{1};
    bool visible{true};
    // Null while the type is not registered (for example a module panel
    // whose module failed to load); the area keeps its place until it is.
    // Shared so a frame loop can keep a panel alive while an operator it
    // runs closes or retypes its own area.
    std::shared_ptr<EditorPanel> panel;
    // The panel's per-area view state (EditorPanel::areaState) as stored in
    // workspaces.json: handed to the panel when it is created, and kept as is
    // while the area has no panel. A live panel's own state supersedes it.
    std::string storedState;
    // ImGui dock node the window was in when last drawn (0 when floating or
    // never drawn). Runtime only; the layout itself is saved by ImGui.
    uint32_t dockId{0};
    // Dock node to move the window into the next time it is drawn, so a new
    // or retyped area takes another area's place. 0 for none.
    uint32_t dockRequest{0};
    // Undock the window the next time it is drawn and float it centred over
    // the main viewport (the floating Play target). Runtime only; ignored
    // while a dockRequest is pending.
    bool floatRequest{false};

    std::string id() const;
    std::string windowName(std::string_view title) const;
    // The panel's current per-area state, or storedState without a panel.
    std::string state() const;
};

// Builds the id "<type>.<number>".
std::string areaId(std::string_view typeId, uint32_t number);
