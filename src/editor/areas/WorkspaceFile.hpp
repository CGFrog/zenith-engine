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
//   "version": 1,
//   "activeWorkspace": "Layout",
//   "workspaces": [
//     {"name": "Layout",
//      "areas": [{"type": "hierarchy", "instance": 1, "visible": true}, ...],
//      "imguiLayout": "[Window][Hierarchy##hierarchy.1]\n..."}
//   ]
// }

struct AreaRecord {
    std::string type;
    uint32_t instance{1};
    bool visible{true};

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

    explicit operator bool() const {
        return success;
    }
};

// Reads the file into contents. A missing file succeeds with found false; an
// invalid file fails and leaves contents untouched.
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
