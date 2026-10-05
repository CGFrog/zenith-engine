// The prefab document cache: each prefab file parsed, converted to the scene
// document entity form, and checked once per file version, then shared by
// every instance built from it.
#include "PrefabSystemInternal.hpp"

#include <chrono>
#include <system_error>

using namespace prefab_detail;

namespace {
// The file's write time and size from one stat, or false when it is not a
// readable regular file.
bool fileVersion(const std::filesystem::path& path, std::filesystem::file_time_type& writeTime, std::uintmax_t& size) {
    std::error_code error;
    const std::filesystem::directory_entry file(path, error);
    if (error || !file.is_regular_file(error) || error) {
        return false;
    }
    writeTime = file.last_write_time(error);
    if (error) {
        return false;
    }
    size = file.file_size(error);
    return !error;
}

// The document's entities in the scene document entity form, keyed by
// prefab-local id; empty, with error set, when one cannot be converted.
std::map<uint64_t, Json> convertEntities(const PrefabDocument& document, std::string& error) {
    std::map<uint64_t, Json> entities;
    for (const PrefabEntity& entity : document.entities) {
        Json& json = entities[entity.id.value];
        if (PrefabResult encoded = prefabEntityToJson(entity, json); !encoded) {
            error = encoded.error;
            return {};
        }
        // Older prefabs may hold collection ids of the scene they came from.
        stripNonContentComponents(json);
    }
    return entities;
}

// Whether the entities, as they are, read through the component codecs: they
// are created with their prefab-local ids in a staging scene.
bool contentReads(const std::map<uint64_t, Json>& entities, AssetManager& assets, const SceneDocumentOptions& options) {
    std::vector<SceneEntityDocument> documents;
    documents.reserve(entities.size());
    for (const auto& [local, json] : entities) {
        documents.push_back({.id = {local}, .entity = &json});
    }
    Scene staging;
    return SceneDocument::createEntities(documents, assets, options, staging).success;
}
} // namespace

namespace prefab_detail {
void destroyEntitiesFrom(Scene& scene, uint64_t firstCreated) {
    std::vector<Entity> created;
    for (const auto [entity, id] : scene.registry().view<const SceneEntityIdComponent>().each()) {
        if (id.id.value >= firstCreated) {
            created.push_back(entity);
        }
    }
    // A created entity's descendants were created too, so destroying one
    // (which takes its descendants) never removes an older entity.
    for (const Entity entity : created) {
        if (scene.registry().valid(entity)) {
            scene.destroyEntity(entity);
        }
    }
}
} // namespace prefab_detail

std::shared_ptr<const PrefabSystem::CachedPrefab> PrefabSystem::cached(const std::filesystem::path& path) {
    const std::string key = path.generic_string();
    const auto now = std::chrono::steady_clock::now();
    const auto found = m_cache.find(key);
    // A stat costs more than building a small prefab, so a recently checked
    // entry is trusted.
    if (found != m_cache.end() && now - found->second->checked < m_fileCheckInterval) {
        ++m_cacheStats.hits;
        return found->second;
    }
    std::filesystem::file_time_type writeTime{};
    std::uintmax_t size = 0;
    const bool exists = fileVersion(path, writeTime, size);
    if (exists && found != m_cache.end() && found->second->writeTime == writeTime && found->second->size == size) {
        found->second->checked = now;
        ++m_cacheStats.hits;
        return found->second;
    }
    // Stamped with the version read before the file, so a write racing the
    // read is caught by the next lookup.
    auto entry = std::make_shared<CachedPrefab>();
    entry->writeTime = writeTime;
    entry->size = size;
    entry->checked = now;
    entry->loaded = loadPrefab(path, entry->document);
    if (entry->loaded) {
        entry->rootLocal = rootId(entry->document).value_or(0);
        entry->entities = convertEntities(entry->document, entry->conversionError);
        entry->contentValid = entry->conversionError.empty() && contentReads(entry->entities, m_assets, m_options);
    }
    ++m_cacheStats.loads;
    if (exists) {
        m_cache.insert_or_assign(key, entry);
    } else {
        // A missing file is looked for again on every use.
        m_cache.erase(key);
    }
    return entry;
}

bool PrefabSystem::cachedGraphValid(const std::filesystem::path& reference, std::span<const PrefabOverride> overrides,
                                    std::vector<std::filesystem::path>& stack) {
    std::filesystem::path path;
    if (!resolve(reference, path) || std::ranges::find(stack, path) != stack.end()) {
        return false;
    }
    const std::shared_ptr<const CachedPrefab> source = cached(path);
    if (!source->loaded || !source->conversionError.empty() || !source->contentValid) {
        return false;
    }
    const std::unordered_set<uint64_t> removed = removedRecords(overrides);
    stack.push_back(path);
    for (const NestedPrefab& record : source->document.nested) {
        if (removed.contains(record.id)) {
            continue;
        }
        if (!cachedGraphValid((reference.parent_path() / record.source).lexically_normal(), record.overrides, stack)) {
            stack.pop_back();
            return false;
        }
    }
    stack.pop_back();
    return true;
}

void PrefabSystem::invalidate(const std::filesystem::path& prefab) {
    std::filesystem::path path;
    if (prefab.is_absolute()) {
        path = prefab.lexically_normal();
    } else if (!resolve(prefab, path)) {
        return;
    }
    m_cache.erase(path.generic_string());
}

void PrefabSystem::clearCache() {
    m_cache.clear();
}

void PrefabSystem::setFileCheckInterval(std::chrono::milliseconds interval) {
    m_fileCheckInterval = interval;
}

PrefabCacheStats PrefabSystem::cacheStats() const {
    PrefabCacheStats stats = m_cacheStats;
    stats.entries = m_cache.size();
    return stats;
}
