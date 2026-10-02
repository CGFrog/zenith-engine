#include "SceneAssetReferences.hpp"

#include "../assets/AssetDatabase.hpp"
#include "../assets/Assets.hpp"

#include <nlohmann/json.hpp>

#include <optional>

namespace {
using Json = nlohmann::json;

constexpr const char* pathKey = "path";
constexpr const char* guidKey = "guid";

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

// Where the asset root lies inside the asset database's root: empty when they
// are the same directory, nullopt when there is no database or it does not
// contain the asset root.
std::optional<std::filesystem::path> assetRootInDatabase(const SceneDocumentOptions& options) {
    if (!options.assetDatabase) {
        return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::path assetRoot = std::filesystem::weakly_canonical(options.assetRoot, error);
    if (error) {
        return std::nullopt;
    }
    const std::filesystem::path databaseRoot = std::filesystem::weakly_canonical(options.assetDatabase->root(), error);
    if (error) {
        return std::nullopt;
    }
    const std::filesystem::path relative = assetRoot.lexically_relative(databaseRoot);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return std::nullopt;
    }
    return relative == "." ? std::filesystem::path{} : relative;
}

std::optional<AssetGuid> guidForReference(const std::string& path, const SceneDocumentOptions& options) {
    const std::optional<std::filesystem::path> prefix = assetRootInDatabase(options);
    if (!prefix) {
        return std::nullopt;
    }
    return options.assetDatabase->guidForPath(toUtf8((*prefix / fromUtf8(path)).lexically_normal()));
}

// The project-relative path of the asset the database knows by guid, or
// nullopt when it does not know the GUID or the asset lies outside the asset
// root.
std::optional<std::string> pathForGuid(AssetGuid guid, const SceneDocumentOptions& options) {
    const std::optional<std::filesystem::path> prefix = assetRootInDatabase(options);
    if (!prefix) {
        return std::nullopt;
    }
    const std::optional<std::string> databasePath = options.assetDatabase->pathForGuid(guid);
    if (!databasePath) {
        return std::nullopt;
    }
    const std::filesystem::path relative = fromUtf8(*databasePath).lexically_relative(*prefix);
    if (relative.empty() || *relative.begin() == "..") {
        return std::nullopt;
    }
    return toUtf8(relative);
}

size_t relocateReferences(Json& value, const std::map<std::string, std::string>& movedPaths) {
    size_t relocated = 0;
    if (value.is_object()) {
        if (const auto path = value.find(pathKey); path != value.end() && path->is_string()) {
            if (const auto moved = movedPaths.find(path->get<std::string>()); moved != movedPaths.end()) {
                *path = moved->second;
                ++relocated;
            }
        }
        for (Json& child : value) {
            relocated += relocateReferences(child, movedPaths);
        }
    } else if (value.is_array()) {
        for (Json& child : value) {
            relocated += relocateReferences(child, movedPaths);
        }
    }
    return relocated;
}
} // namespace

SceneDocumentResult projectRelativeAssetPath(const std::filesystem::path& source, const SceneDocumentOptions& options,
                                             std::string& output) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(options.assetRoot, error);
    if (error) {
        return {.error = "Could not resolve asset root: " + error.message()};
    }
    const std::filesystem::path canonicalSource = std::filesystem::weakly_canonical(source, error);
    if (error) {
        return {.error = "Could not resolve asset path " + source.string() + ": " + error.message()};
    }
    const std::filesystem::path relative = canonicalSource.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {.error = "Asset is outside the project asset root: " + canonicalSource.string()};
    }
    output = relative.generic_string();
    return {.success = true};
}

SceneDocumentResult resolveProjectAssetPath(std::string_view reference, const SceneDocumentOptions& options,
                                            std::filesystem::path& output) {
    const std::filesystem::path relative(reference);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {.error = "Asset reference must be project-relative: " + std::string(reference)};
    }
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(options.assetRoot, error);
    if (error) {
        return {.error = "Could not resolve asset root: " + error.message()};
    }
    output = std::filesystem::weakly_canonical(root / relative, error);
    if (error || !std::filesystem::is_regular_file(output, error)) {
        return {.error = "Missing asset: " + (root / relative).string()};
    }
    const std::filesystem::path verifiedRelative = output.lexically_relative(root);
    if (verifiedRelative.empty() || verifiedRelative.is_absolute() || *verifiedRelative.begin() == "..") {
        return {.error = "Asset reference escapes the project asset root: " + std::string(reference)};
    }
    return {.success = true};
}

void writeAssetReference(const std::string& path, const SceneDocumentOptions& options, Json& reference) {
    reference[pathKey] = path;
    if (const std::optional<AssetGuid> guid = guidForReference(path, options)) {
        reference[guidKey] = guid->toString();
    } else {
        reference.erase(guidKey);
    }
}

SceneDocumentResult readAssetReference(const Json& reference, const SceneDocumentOptions& options, std::string& path) {
    if (reference.is_string()) {
        path = reference.get<std::string>();
        return {.success = true};
    }
    if (!reference.is_object()) {
        return {.error = "An asset reference must be an object or a path string"};
    }
    const auto storedPath = reference.find(pathKey);
    if (storedPath != reference.end() && !storedPath->is_string()) {
        return {.error = "An asset reference path must be a string"};
    }
    if (const auto storedGuid = reference.find(guidKey); storedGuid != reference.end()) {
        const std::optional<AssetGuid> guid =
            storedGuid->is_string() ? AssetGuid::parse(storedGuid->get<std::string>()) : std::nullopt;
        if (!guid || !*guid) {
            return {.error = "An asset reference guid must be 32 hexadecimal digits"};
        }
        if (std::optional<std::string> current = pathForGuid(*guid, options)) {
            path = std::move(*current);
            return {.success = true};
        }
    }
    if (storedPath == reference.end()) {
        return {.error = "Asset reference has no path and its guid is not a known asset"};
    }
    path = storedPath->get<std::string>();
    return {.success = true};
}

SceneDocumentResult relocateSceneAssetReferences(std::string_view document,
                                                 const std::map<std::string, std::string>& movedPaths,
                                                 std::string& output, size_t& relocated) {
    relocated = 0;
    try {
        Json parsed = Json::parse(document);
        if (!parsed.is_object() || !parsed.contains("entities") || !parsed["entities"].is_array()) {
            return {.error = "Scene document must contain version and entities fields"};
        }
        for (Json& entity : parsed["entities"]) {
            if (const auto components = entity.find("components"); components != entity.end()) {
                relocated += relocateReferences(*components, movedPaths);
            }
        }
        output = relocated == 0 ? std::string(document) : parsed.dump(2) + '\n';
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Invalid scene document: " + std::string(error.what())};
    }
}

SceneDocumentResult projectTextureReference(const AssetManager& assets, TextureHandle handle,
                                            const std::string& authored, const SceneDocumentOptions& options,
                                            std::string& output) {
    output = authored;
    if (!handle) {
        return {.success = true};
    }
    const auto source = assets.textureSource(handle);
    if (!source) {
        return {.error = "Cannot save a texture reference with an unknown runtime handle"};
    }
    return projectRelativeAssetPath(source->path, options, output);
}

SceneDocumentResult loadProjectTexture(const std::string& reference, AssetManager& assets,
                                       const SceneDocumentOptions& options, TextureHandle& output) {
    output = {};
    if (reference.empty()) {
        return {.success = true};
    }
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveProjectAssetPath(reference, options, path); !result) {
        return result;
    }
    const AssetLoadResult<TextureHandle> loaded = assets.loadTexture(path);
    if (!loaded) {
        return {.error = loaded.error};
    }
    output = loaded.handle;
    return {.success = true};
}
