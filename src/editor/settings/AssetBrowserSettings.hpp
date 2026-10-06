#pragma once

// Asset browser preferences kept in asset-browser.json in the editor's settings directory.

#include <filesystem>
#include <string>
#include <vector>

enum class AssetBrowserViewMode {
    Grid,
    List,
};

// Asset browser preferences kept in asset-browser.json in the editor's
// settings directory. Folder paths are UTF-8, '/'-separated, and relative to
// the project's asset root ("." is the root itself; "../scenes" reaches a
// sibling scene root).
struct AssetBrowserSettings {
    AssetBrowserViewMode viewMode{AssetBrowserViewMode::Grid};
    std::vector<std::string> favourites;
    std::string lastFolder;

    bool operator==(const AssetBrowserSettings&) const = default;
};

struct AssetBrowserSettingsResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// A missing file leaves the defaults and succeeds. An unreadable or invalid
// file also leaves the defaults, and reports why.
AssetBrowserSettingsResult loadAssetBrowserSettings(const std::filesystem::path& path, AssetBrowserSettings& settings);
AssetBrowserSettingsResult saveAssetBrowserSettings(const std::filesystem::path& path,
                                                    const AssetBrowserSettings& settings);
