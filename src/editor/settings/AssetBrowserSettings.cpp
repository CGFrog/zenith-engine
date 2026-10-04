#include "AssetBrowserSettings.hpp"

#include "engine/platform/VersionedJson.hpp"

#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;

const VersionedJsonFormat assetBrowserSettingsFormat{.description = "asset browser settings"};

const char* viewModeName(AssetBrowserViewMode mode) {
    return mode == AssetBrowserViewMode::List ? "list" : "grid";
}

AssetBrowserSettingsResult parse(const Json& document, AssetBrowserSettings& settings) {
    AssetBrowserSettings parsed;
    const auto viewModeEntry = document.find("viewMode");
    if (viewModeEntry != document.end() && !viewModeEntry->is_string()) {
        return {.error = "viewMode must be \"grid\" or \"list\""};
    }
    const std::string viewMode = viewModeEntry == document.end() ? "grid" : viewModeEntry->get<std::string>();
    if (viewMode == "list") {
        parsed.viewMode = AssetBrowserViewMode::List;
    } else if (viewMode != "grid") {
        return {.error = "unknown view mode '" + viewMode + "'"};
    }
    if (const auto favourites = document.find("favourites"); favourites != document.end()) {
        if (!favourites->is_array()) {
            return {.error = "favourites must be an array of folder paths"};
        }
        for (const Json& favourite : *favourites) {
            if (!favourite.is_string()) {
                return {.error = "favourites must be an array of folder paths"};
            }
            parsed.favourites.push_back(favourite.get<std::string>());
        }
    }
    if (const auto lastFolder = document.find("lastFolder"); lastFolder != document.end()) {
        if (!lastFolder->is_string()) {
            return {.error = "lastFolder must be a folder path"};
        }
        parsed.lastFolder = lastFolder->get<std::string>();
    }
    settings = std::move(parsed);
    return {.success = true};
}
} // namespace

AssetBrowserSettingsResult loadAssetBrowserSettings(const std::filesystem::path& path, AssetBrowserSettings& settings) {
    settings = {};
    const VersionedJsonResult read = readVersionedJson(path, assetBrowserSettingsFormat);
    if (!read) {
        return {.error = read.error + "; using defaults."};
    }
    if (!read.found) {
        return {.success = true};
    }
    if (AssetBrowserSettingsResult result = parse(read.document, settings); !result) {
        return {.error =
                    "Invalid asset browser settings in " + path.string() + " (" + result.error + "); using defaults."};
    }
    return {.success = true};
}

AssetBrowserSettingsResult saveAssetBrowserSettings(const std::filesystem::path& path,
                                                    const AssetBrowserSettings& settings) {
    const Json document{{"viewMode", viewModeName(settings.viewMode)},
                        {"favourites", settings.favourites},
                        {"lastFolder", settings.lastFolder}};
    if (const PlatformResult written = writeVersionedJson(path, assetBrowserSettingsFormat, document); !written) {
        return {.error = written.error};
    }
    return {.success = true};
}
