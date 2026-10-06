#pragma once

// The recent projects list, persisted as recent-projects.json in the editor settings directory.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

struct RecentProject {
    // Absolute and normalized.
    std::filesystem::path projectFile;
    // The project's display name when it was last opened.
    std::string displayName;
    std::chrono::system_clock::time_point lastOpened;
};

struct RecentProjectsResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Whether the entry's project file no longer exists, so the hub flags it and
// offers to remove it instead of opening it.
bool recentProjectMissing(const RecentProject& project);

// The projects the editor opened, most recent first, persisted as
// recent-projects.json in the editor settings directory:
//
//   {"version": 1, "projects": [{"path": "...", "name": "...",
//                                "lastOpened": <seconds since the Unix epoch>}]}
//
// Paths are compared after making them absolute and normalized (and, on
// Windows, case-insensitively), so one project is listed once however it was
// named. The list keeps at most maximumEntries projects.
class RecentProjects {
  public:
    static constexpr size_t maximumEntries = 20;
    static constexpr int currentVersion = 1;

    explicit RecentProjects(std::filesystem::path file);

    // A missing file is an empty list. A malformed one is an error and leaves
    // the list empty; entries with invalid fields are skipped.
    RecentProjectsResult load();
    RecentProjectsResult save() const;

    // Moves the project to the front (adding it when new) with the given
    // time, dropping the oldest entries beyond maximumEntries.
    void recordOpened(const std::filesystem::path& projectFile, std::string displayName,
                      std::chrono::system_clock::time_point openedAt);
    // Returns whether an entry was removed.
    bool remove(const std::filesystem::path& projectFile);

    std::span<const RecentProject> entries() const;
    const std::filesystem::path& file() const;

  private:
    std::filesystem::path m_file;
    std::vector<RecentProject> m_entries;
};
