#pragma once

#include "editor/project/AssetCatalog.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct EditorContext;

// What an asset field needs beyond its value: the project's files to offer
// and, optionally, their thumbnails. It holds references only and lives for
// one draw.
struct AssetReferenceFieldContext {
    const AssetCatalog& catalog;
    // Field values are relative to this folder (the scene document's asset
    // root), with forward slashes; assets outside it are not offered.
    std::filesystem::path assetRoot;
    // The ImGui texture id of an image asset's thumbnail (absolute path), or
    // 0 while there is none; unset draws no thumbnails.
    std::function<uint64_t(const std::filesystem::path&)> thumbnail;
};

// The context for asset fields in the editor: the session's project assets
// and GPU thumbnails. Null when the context has no project (some tests).
std::optional<AssetReferenceFieldContext> assetReferenceFieldContext(EditorContext& context);

// The catalog kinds a FieldPresentation::assetKind accepts: "texture" and
// "environment" take images, "audio" sounds, "material" .zmat files, "model"
// glTF models, and "asset" (or any word the editor does not know) every kind.
std::vector<EditorAssetKind> editorAssetKindsFor(std::string_view assetKind);

// The drag payload type the Asset Browser gives an asset of kind, or null for
// kinds dragged only to move them (scenes, scripts).
const char* assetDragPayloadFor(EditorAssetKind kind);

// The field value naming an absolute path: relative to assetRoot with forward
// slashes, in UTF-8. nullopt when the path lies outside assetRoot.
std::optional<std::string> projectAssetValue(const std::filesystem::path& assetRoot,
                                             const std::filesystem::path& absolutePath);

// The catalog assets a field of assetKind offers, those whose file name
// contains search (ignoring case), sorted by value.
std::vector<const EditorAssetRecord*> assetFieldCandidates(const AssetReferenceFieldContext& context,
                                                           std::string_view assetKind, std::string_view search);

enum class AssetReferenceState : uint8_t {
    None,
    Present,
    // The value names no file in the project, or the asset failed to load.
    Missing,
};

// None for an empty value, Missing when the value names no asset of the
// catalog or loaded is false, otherwise Present.
AssetReferenceState assetReferenceState(const AssetReferenceFieldContext& context, const std::string& value,
                                        std::optional<bool> loaded);

struct AssetReferenceFieldOptions {
    // The asset's load state (ComponentType::assetLoaded); nullopt when the
    // field has no loaded handle.
    std::optional<bool> loaded;
    // The objects being edited hold different values: the box shows
    // mixedValueText, and any choice applies to all of them.
    bool mixed{false};
    // Offer the Clear button; a field that must name an asset turns it off.
    bool clearable{true};
};

// Draws one asset field: a box naming the asset's file (red when it is
// missing, the full path in its tooltip) that opens a searchable picker of
// matching project assets with thumbnails and accepts a matching asset
// dragged from the Asset Browser, a Clear button, and label. Edits value (a
// project-relative path) in place and returns whether it changed; the caller
// records undo. Its items (##value, Clear) take the current id stack, so
// callers push an id per field.
bool drawAssetReferenceField(const char* label, std::string& value, std::string_view assetKind,
                             const AssetReferenceFieldContext& context, const AssetReferenceFieldOptions& options = {});
