#include "OrphanData.hpp"

#include "engine/scene/Scene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_set>

namespace {
using Json = nlohmann::json;

void collectReferences(const Json& value, SceneAssetUsage& usage) {
    if (value.is_object()) {
        if (const auto path = value.find("path"); path != value.end() && path->is_string()) {
            usage.paths.insert(path->get<std::string>());
            if (const auto guid = value.find("guid"); guid != value.end() && guid->is_string()) {
                if (const std::optional<AssetGuid> parsed = AssetGuid::parse(guid->get<std::string>())) {
                    usage.guids.insert(*parsed);
                }
            }
        }
        for (const Json& child : value) {
            collectReferences(child, usage);
        }
    } else if (value.is_array()) {
        for (const Json& child : value) {
            collectReferences(child, usage);
        }
    }
}

bool referenceable(EditorAssetKind kind) {
    return kind != EditorAssetKind::Scene;
}
} // namespace

bool collectSceneAssetUsage(std::string_view document, SceneAssetUsage& usage, std::string& error) {
    usage = {};
    try {
        const Json parsed = Json::parse(document);
        const auto entities = parsed.is_object() ? parsed.find("entities") : parsed.end();
        if (!parsed.is_object() || entities == parsed.end() || !entities->is_array()) {
            error = "Scene document must contain an entities array";
            return false;
        }
        for (const Json& entity : *entities) {
            if (const auto components = entity.find("components"); components != entity.end()) {
                collectReferences(*components, usage);
            }
        }
        return true;
    } catch (const Json::exception& exception) {
        error = "Invalid scene document: " + std::string(exception.what());
        return false;
    }
}

std::vector<OrphanAsset> findOrphanAssets(std::span<const EditorAssetRecord> assets,
                                          const std::filesystem::path& assetRoot, const SceneAssetUsage& usage) {
    const std::filesystem::path root = assetRoot.lexically_normal();
    std::vector<OrphanAsset> orphans;
    for (const EditorAssetRecord& asset : assets) {
        if (!referenceable(asset.kind)) {
            continue;
        }
        const std::filesystem::path relative = asset.absolutePath.lexically_normal().lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") {
            continue;
        }
        std::string path = relative.generic_string();
        if (usage.paths.contains(path) || (asset.guid && usage.guids.contains(asset.guid))) {
            continue;
        }
        orphans.push_back({.path = std::move(path), .kind = asset.kind});
    }
    std::ranges::sort(orphans, {}, &OrphanAsset::path);
    return orphans;
}

std::vector<SceneCollectionId> findEmptyCollections(const Scene& scene) {
    std::unordered_set<uint64_t> used;
    for (const auto& [entity, membership] : scene.registry().view<const CollectionMembershipComponent>().each()) {
        for (const SceneCollectionId id : membership.collections) {
            used.insert(id.value);
        }
    }
    const SceneCollections& collections = scene.collections();
    std::vector<SceneCollectionId> empty;
    for (const SceneCollection& collection : collections.all()) {
        const std::vector<SceneCollectionId> subtree = collections.subtree(collection.id);
        if (std::ranges::none_of(subtree, [&used](SceneCollectionId id) { return used.contains(id.value); })) {
            empty.push_back(collection.id);
        }
    }
    return empty;
}
