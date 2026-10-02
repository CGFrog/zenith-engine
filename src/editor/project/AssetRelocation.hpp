#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

// One project file that now lives somewhere else. Both paths are absolute and
// lexically normal. Directory moves are reported per file.
struct ProjectFileMove {
    std::filesystem::path from;
    std::filesystem::path to;
};

// Where path is after the moves, or nullopt when none of them moved it.
std::optional<std::filesystem::path> relocatedProjectPath(const std::filesystem::path& path,
                                                          std::span<const ProjectFileMove> moves);

// Whether path is one of removed (absolute and lexically normal).
bool isRemovedProjectPath(const std::filesystem::path& path, std::span<const std::filesystem::path> removed);

// Points the scene's asset references at the new locations of moved files, so
// the scene keeps loading and saving them after a rename made in the asset
// browser or outside the editor. Only entities that reference a moved asset
// are rebuilt (SceneDocument::applyChanges); the others keep their handles.
// relocated counts the references changed.
SceneDocumentResult relocateSceneAssets(Scene& scene, AssetManager& assets, const SceneDocumentOptions& options,
                                        std::span<const ProjectFileMove> moves, size_t& relocated);
