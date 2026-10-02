#pragma once

#include "SceneDocument.hpp"

#include <filesystem>
#include <string>
#include <string_view>

// Scene documents reference assets by paths relative to the project asset
// root, written with forward slashes. These helpers convert between those
// references and loaded assets for component codecs.

// The project-relative reference for source; fails when source lies outside
// the asset root.
SceneDocumentResult projectRelativeAssetPath(const std::filesystem::path& source, const SceneDocumentOptions& options,
                                             std::string& output);

// Resolves a project-relative reference to an existing file inside the asset
// root; rejects absolute paths and references that escape the root.
SceneDocumentResult resolveProjectAssetPath(std::string_view reference, const SceneDocumentOptions& options,
                                            std::filesystem::path& output);

// The reference to save for an optional texture: the loaded handle's source
// when there is one, else the authored path (which may name a missing file).
SceneDocumentResult projectTextureReference(const AssetManager& assets, TextureHandle handle,
                                            const std::string& authored, const SceneDocumentOptions& options,
                                            std::string& output);

// Loads the texture a reference names; an empty reference leaves output empty.
SceneDocumentResult loadProjectTexture(const std::string& reference, AssetManager& assets,
                                       const SceneDocumentOptions& options, TextureHandle& output);
