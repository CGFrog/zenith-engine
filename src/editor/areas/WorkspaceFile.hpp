#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class WorkspaceSet;

// workspaces.json: the user's workspaces, their areas, and each workspace's
// ImGui docking layout. It is editor user data and never part of a scene.
//
// {
//   "version": 3,
//   "activeWorkspace": "Layout",
//   "workspaces": [
//     {"name": "Layout",
//      "areas": [{"type": "hierarchy", "instance": 1, "visible": true},
//                {"type": "properties", "instance": 1, "visible": true, "state": "world"}, ...],
//      "imguiLayout": "[Window][Hierarchy##hierarchy.1]\n..."}
//   ]
// }
//
// The version changes whenever the default workspaces change, so files from
// older editors (which hold the old defaults, usually untouched) are replaced
// by the new defaults rather than migrated. Version 1 (phase 5 development)
// had the same format without "state"; version 3 added the System Profiler
// to the Debug workspace.

// The version saveWorkspaceFile writes.
inline constexpr int currentWorkspaceFileVersion = 3;

struct AreaRecord {
    std::string type;
    uint32_t instance{1};
    bool visible{true};
    // The panel's per-area view state (EditorPanel::areaState); empty for
    // none, and then omitted from the file.
    std::string state;

    bool operator==(const AreaRecord&) const = default;
};

struct WorkspaceRecord {
    std::string name;
    std::vector<AreaRecord> areas;
    // ImGui settings text; empty means the workspace's default layout.
    std::string imguiLayout;

    bool operator==(const WorkspaceRecord&) const = default;
};

struct WorkspaceFileContents {
    std::vector<WorkspaceRecord> workspaces;
    std::string activeWorkspace;

    bool operator==(const WorkspaceFileContents&) const = default;
};

struct WorkspaceFileResult {
    bool success{false};
    std::string error;
    // False when loading found no file; the contents are then untouched.
    bool found{true};
    // True when loading found a file from an older version, whose default
    // workspaces this editor has replaced; the contents are then untouched
    // and the caller starts from the current defaults.
    bool outdated{false};

    explicit operator bool() const {
        return success;
    }
};

// Reads the file into contents. A missing file succeeds with found false; an
// older version succeeds with outdated true; an invalid file or a newer
// version fails. Only a current file changes contents.
WorkspaceFileResult loadWorkspaceFile(const std::filesystem::path& path, WorkspaceFileContents& contents);
// Writes through a temporary file, so an interrupted save keeps the old file.
WorkspaceFileResult saveWorkspaceFile(const std::filesystem::path& path, const WorkspaceFileContents& contents);

// The records of the live workspaces, including the active one's layout as
// last captured.
WorkspaceFileContents captureWorkspaces(const WorkspaceSet& workspaces);
// Replaces the live workspaces with the records; their panels are created by
// the next sync. Invalid areas are skipped and reported in warnings.
void restoreWorkspaces(const WorkspaceFileContents& contents, WorkspaceSet& workspaces,
                       std::vector<std::string>& warnings);
