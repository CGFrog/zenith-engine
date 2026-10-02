#include "AssetBrowserSettings.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace {
using Json = nlohmann::json;

constexpr int currentVersion = 1;

const char* viewModeName(AssetBrowserViewMode mode) {
    return mode == AssetBrowserViewMode::List ? "list" : "grid";
}

AssetBrowserSettingsResult parse(const std::string& text, AssetBrowserSettings& settings) {
    const Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {.error = "the file is not a JSON object"};
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != currentVersion) {
        return {.error = "unsupported version (expected " + std::to_string(currentVersion) + ")"};
    }
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
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return {.error = "Cannot inspect asset browser settings " + path.string() + ": " + error.message()};
        }
        return {.success = true};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {.error = "Cannot read asset browser settings " + path.string() + "; using defaults."};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (AssetBrowserSettingsResult result = parse(contents.str(), settings); !result) {
        return {.error =
                    "Invalid asset browser settings in " + path.string() + " (" + result.error + "); using defaults."};
    }
    return {.success = true};
}

AssetBrowserSettingsResult saveAssetBrowserSettings(const std::filesystem::path& path,
                                                    const AssetBrowserSettings& settings) {
    const Json document{{"version", currentVersion},
                        {"viewMode", viewModeName(settings.viewMode)},
                        {"favourites", settings.favourites},
                        {"lastFolder", settings.lastFolder}};
    std::error_code error;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return {.error = "Cannot create the asset browser settings directory: " + error.message()};
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    // Replace invalid UTF-8 rather than throwing from dump().
    output << document.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    if (!output) {
        return {.error = "Cannot write asset browser settings " + path.string()};
    }
    return {.success = true};
}
