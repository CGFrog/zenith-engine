#include "AssetRelocation.hpp"

#include "engine/scene/SceneAssetReferences.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {
// The project-relative reference SceneDocument writes for path, or empty when
// path lies outside the asset root.
std::string assetReferenceFor(const std::filesystem::path& path, const std::filesystem::path& canonicalRoot) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return {};
    }
    const std::filesystem::path relative = canonical.lexically_relative(canonicalRoot);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {};
    }
    return relative.generic_string();
}
} // namespace

std::optional<std::filesystem::path> relocatedProjectPath(const std::filesystem::path& path,
                                                          std::span<const ProjectFileMove> moves) {
    const auto found = std::ranges::find(moves, path, &ProjectFileMove::from);
    if (found == moves.end()) {
        return std::nullopt;
    }
    return found->to;
}

bool isRemovedProjectPath(const std::filesystem::path& path, std::span<const std::filesystem::path> removed) {
    return std::ranges::find(removed, path) != removed.end();
}

SceneDocumentResult relocateSceneAssets(Scene& scene, AssetManager& assets, const SceneDocumentOptions& options,
                                        std::span<const ProjectFileMove> moves, size_t& relocated) {
    relocated = 0;
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(options.assetRoot, error);
    if (error) {
        return {.error = "Could not resolve asset root: " + error.message()};
    }
    std::map<std::string, std::string> movedReferences;
    for (const ProjectFileMove& move : moves) {
        std::string from = assetReferenceFor(move.from, root);
        std::string to = assetReferenceFor(move.to, root);
        if (!from.empty() && !to.empty()) {
            movedReferences.emplace(std::move(from), std::move(to));
        }
    }
    if (movedReferences.empty()) {
        return {.success = true};
    }

    std::string before;
    if (SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, before); !result) {
        return result;
    }
    std::string after;
    if (SceneDocumentResult result = relocateSceneAssetReferences(before, movedReferences, after, relocated);
        !result || relocated == 0) {
        return result;
    }
    std::vector<SceneEntityChange> changes;
    if (SceneDocumentResult result = SceneDocument::diff(before, after, changes); !result) {
        return result;
    }
    return SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, scene);
}
