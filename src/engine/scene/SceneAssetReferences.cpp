#include "SceneAssetReferences.hpp"

#include "../assets/Assets.hpp"

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
