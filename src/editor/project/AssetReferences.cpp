#include "AssetReferences.hpp"

#include "editor/project/ProjectFileOperations.hpp"
#include "engine/assets/MaterialAssetFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {
using Json = nlohmann::json;

constexpr std::string_view legacyAssetKeySuffix = "_asset";

// True when path is base or lies inside it.
bool isWithin(const std::filesystem::path& path, const std::filesystem::path& base) {
    const std::filesystem::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

void addPath(const std::filesystem::path& base, std::string_view utf8Path, FileAssetReferences& output) {
    if (!utf8Path.empty()) {
        output.paths.insert(normalizedCatalogPath(base / pathFromUtf8Text(utf8Path)));
    }
}

void addGuid(const Json& value, FileAssetReferences& output) {
    if (value.is_string()) {
        if (const std::optional<AssetGuid> guid = AssetGuid::parse(value.get<std::string>())) {
            output.guids.insert(*guid);
        }
    }
}

// Collects component asset references at any depth: reference objects (the
// key `path` is reserved for them, see SceneAssetReferences.hpp) and legacy
// path strings under keys ending in `_asset`.
void collectComponentReferences(const Json& value, const std::filesystem::path& assetRoot,
                                 FileAssetReferences& output) {
    if (value.is_object()) {
        if (const auto path = value.find("path"); path != value.end() && path->is_string()) {
            addPath(assetRoot, path->get<std::string>(), output);
            if (const auto guid = value.find("guid"); guid != value.end()) {
                addGuid(*guid, output);
            }
        }
        for (auto item = value.begin(); item != value.end(); ++item) {
            if (item.value().is_string() && item.key().ends_with(legacyAssetKeySuffix)) {
                addPath(assetRoot, item.value().get<std::string>(), output);
            } else {
                collectComponentReferences(item.value(), assetRoot, output);
            }
        }
    } else if (value.is_array()) {
        for (const Json& child : value) {
            collectComponentReferences(child, assetRoot, output);
        }
    }
}

std::string collectDocumentReferences(EditorAssetKind kind, std::string_view text, const std::filesystem::path& file,
                                      const std::filesystem::path& assetRoot, FileAssetReferences& output) {
    try {
        const Json document = Json::parse(text);
        const auto entities = document.is_object() ? document.find("entities") : document.end();
        if (!document.is_object() || entities == document.end() || !entities->is_array()) {
            return "the document has no entities array";
        }
        for (const Json& entity : *entities) {
            if (const auto components = entity.find("components"); components != entity.end()) {
                collectComponentReferences(*components, assetRoot, output);
            }
        }
        // Nested prefabs name their source relative to the containing prefab.
        if (const auto nested = document.find("nested"); kind == EditorAssetKind::Prefab && nested != document.end() &&
                                                          nested->is_array()) {
            for (const Json& entry : *nested) {
                if (const auto source = entry.find("source"); source != entry.end() && source->is_string()) {
                    addPath(file.parent_path(), source->get<std::string>(), output);
                }
            }
        }
        return {};
    } catch (const Json::exception& error) {
        return std::string("not valid JSON: ") + error.what();
    }
}

std::string collectMaterialReferences(std::string_view text, const std::filesystem::path& file,
                                      FileAssetReferences& output) {
    MaterialFile material;
    if (const MaterialFileResult parsed = parseMaterialFile(text, material); !parsed) {
        return parsed.error;
    }
    for (const MaterialTextureReference& texture : material.textures) {
        addPath(file.parent_path(), texture.path, output);
        if (texture.guid) {
            output.guids.insert(texture.guid);
        }
    }
    return {};
}

bool readText(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    text = std::move(contents).str();
    return true;
}

bool canReference(EditorAssetKind kind) {
    return kind == EditorAssetKind::Scene || kind == EditorAssetKind::Prefab || kind == EditorAssetKind::Material;
}
} // namespace

std::string collectFileAssetReferences(EditorAssetKind kind, std::string_view text, const std::filesystem::path& file,
                                       const std::filesystem::path& assetRoot, FileAssetReferences& output) {
    output = {};
    const std::filesystem::path normalizedFile = normalizedCatalogPath(file);
    if (kind == EditorAssetKind::Scene || kind == EditorAssetKind::Prefab) {
        return collectDocumentReferences(kind, text, normalizedFile, normalizedCatalogPath(assetRoot), output);
    }
    if (kind == EditorAssetKind::Material) {
        return collectMaterialReferences(text, normalizedFile, output);
    }
    return {};
}

bool referencesAsset(const FileAssetReferences& references, const std::filesystem::path& absolutePath,
                     AssetGuid guid) {
    return references.paths.contains(normalizedCatalogPath(absolutePath)) ||
           (guid && references.guids.contains(guid));
}

const AssetReferenceIndex::CachedFile& AssetReferenceIndex::cached(const EditorAssetRecord& record,
                                                                   const std::filesystem::path& assetRoot,
                                                                   std::vector<std::string>& warnings) {
    const auto found = m_files.find(record.absolutePath);
    if (found != m_files.end() && found->second.size == record.byteSize &&
        found->second.writeTime == record.lastWriteTime) {
        return found->second;
    }
    CachedFile file{.size = record.byteSize, .writeTime = record.lastWriteTime};
    std::string text;
    std::string error = readText(record.absolutePath, text) ? std::string{} : "it cannot be read";
    if (error.empty()) {
        error = collectFileAssetReferences(record.kind, text, record.absolutePath, assetRoot, file.references);
    }
    file.readable = error.empty();
    if (!file.readable) {
        warnings.push_back("Find References skipped '" + utf8PathText(record.absolutePath) + "': " + error);
    }
    return m_files.insert_or_assign(record.absolutePath, std::move(file)).first->second;
}

std::vector<AssetReferrer> AssetReferenceIndex::findReferences(const AssetCatalog& catalog,
                                                               const std::filesystem::path& item,
                                                               std::vector<std::string>& warnings) {
    const std::filesystem::path target = normalizedCatalogPath(item);
    const std::filesystem::path assetRoot = catalog.roots().front();
    std::vector<const EditorAssetRecord*> targets;
    for (const EditorAssetRecord& asset : catalog.assets()) {
        if (isWithin(asset.absolutePath, target)) {
            targets.push_back(&asset);
        }
    }
    std::vector<AssetReferrer> referrers;
    if (targets.empty()) {
        return referrers;
    }
    for (const EditorAssetRecord& candidate : catalog.assets()) {
        if (!canReference(candidate.kind) || isWithin(candidate.absolutePath, target)) {
            continue;
        }
        const CachedFile& file = cached(candidate, assetRoot, warnings);
        const bool uses = std::ranges::any_of(targets, [&file](const EditorAssetRecord* asset) {
            return referencesAsset(file.references, asset->absolutePath, asset->guid);
        });
        if (uses) {
            referrers.push_back({.file = candidate.absolutePath, .kind = candidate.kind});
        }
    }
    // Forget files that left the catalog.
    std::erase_if(m_files, [&catalog](const auto& entry) { return catalog.findAsset(entry.first) == nullptr; });
    return referrers;
}

std::vector<SceneEntityId> sceneEntitiesReferencing(std::string_view document, const std::filesystem::path& assetRoot,
                                                    const std::filesystem::path& absolutePath, AssetGuid guid) {
    std::vector<SceneEntityId> entities;
    try {
        const Json parsed = Json::parse(document);
        const auto list = parsed.is_object() ? parsed.find("entities") : parsed.end();
        if (!parsed.is_object() || list == parsed.end() || !list->is_array()) {
            return entities;
        }
        const std::filesystem::path root = normalizedCatalogPath(assetRoot);
        for (const Json& entity : *list) {
            const auto id = entity.find("id");
            const auto components = entity.find("components");
            if (id == entity.end() || !id->is_number_unsigned() || components == entity.end()) {
                continue;
            }
            FileAssetReferences references;
            collectComponentReferences(*components, root, references);
            if (referencesAsset(references, absolutePath, guid)) {
                entities.push_back(SceneEntityId{id->get<uint64_t>()});
            }
        }
    } catch (const Json::exception&) {
        entities.clear();
    }
    return entities;
}
