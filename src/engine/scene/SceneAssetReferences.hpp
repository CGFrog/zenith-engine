#pragma once

// Scene documents reference assets by paths relative to the project asset root, written with forward slashes.

#include "SceneDocument.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

// Scene documents reference assets by paths relative to the project asset
// root, written with forward slashes. These helpers convert between those
// references and loaded assets for component codecs.
//
// A version 2 reference is a JSON object holding the project-relative `path`
// and, when SceneDocumentOptions::assetDatabase knows the asset, its `guid`:
//
//   {"guid": "0f8e...", "path": "textures/stone.png"}
//
// Codecs may add their own keys to the same object (a mesh index, an instance
// root). Inside component JSON the key `path` is reserved for asset
// references, which lets editor tooling relocate references generically.

// The project-relative reference for source; fails when source lies outside
// the asset root.
SceneDocumentResult projectRelativeAssetPath(const std::filesystem::path& source, const SceneDocumentOptions& options,
                                             std::string& output);

// Resolves a project-relative reference to an existing file inside the asset
// root; rejects absolute paths and references that escape the root.
SceneDocumentResult resolveProjectAssetPath(std::string_view reference, const SceneDocumentOptions& options,
                                            std::filesystem::path& output);

// Writes `path`, and `guid` when the asset database knows the asset, into
// reference, which must be a JSON object or null. Other keys are kept.
void writeAssetReference(const std::string& path, const SceneDocumentOptions& options, nlohmann::json& reference);

// The current project-relative path of a reference: a version 2 object, or a
// legacy path string. A GUID the asset database knows wins over the stored
// path, so references to renamed or moved assets follow them; an unknown GUID
// (no database, or an asset outside it) falls back to the path. An empty
// string reads as "no asset".
SceneDocumentResult readAssetReference(const nlohmann::json& reference, const SceneDocumentOptions& options,
                                       std::string& path);

// Rewrites the `path` of every asset reference in a serialized current-version
// document whose path is a key of movedPaths (project-relative, old path to
// new path). The GUID is kept. relocated counts the references changed; output
// is the input unchanged in content when it is zero.
SceneDocumentResult relocateSceneAssetReferences(std::string_view document,
                                                 const std::map<std::string, std::string>& movedPaths,
                                                 std::string& output, size_t& relocated);

// The reference to save for an optional texture: the loaded handle's source
// when there is one, else the authored path (which may name a missing file).
SceneDocumentResult projectTextureReference(const AssetManager& assets, TextureHandle handle,
                                            const std::string& authored, const SceneDocumentOptions& options,
                                            std::string& output);

// Loads the texture a reference names; an empty reference leaves output empty.
SceneDocumentResult loadProjectTexture(const std::string& reference, AssetManager& assets,
                                       const SceneDocumentOptions& options, TextureHandle& output);

// Loads the material file (.zmat, see MaterialAssetFile.hpp) a reference
// names; an empty reference leaves output empty.
SceneDocumentResult loadProjectMaterial(const std::string& reference, AssetManager& assets,
                                        const SceneDocumentOptions& options, MaterialHandle& output);

// Whether a loaded handle still matches a path field after an edit:
// loadedSource is the handle's source file (empty for none or for a runtime
// asset) and loaded whether there is a live handle. An empty reference
// matches no handle, and a runtime asset (loaded without a source) so code
// keeps it; any other reference matches only the loaded file it resolves to.
bool projectAssetStillNamed(const std::string& reference, const std::filesystem::path& loadedSource, bool loaded,
                            const SceneDocumentOptions& options);
