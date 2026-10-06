// Internal to the prefab system: what PrefabSystem.cpp and its domain files
// (PrefabIdRemap, PrefabOverrides, PrefabNesting, PrefabQueries) share. Not
// part of the public engine API.
#pragma once

// Internal to the prefab system: what PrefabSystem.cpp and its domain files (PrefabIdRemap, PrefabOverrides,
// PrefabNesting, PrefabQueries) share.

#include "Prefab.hpp"

#include "../assets/Assets.hpp"
#include "ComponentTypes.hpp"
#include "EntityReferenceRemap.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Instances are compared with and rebuilt from their prefab through the scene
// document entity form: instance entities are serialized exactly as a scene
// save writes them, prefab entities are normalized by loading them into a
// staging scene and serializing them the same way, and instances are rebuilt
// with SceneDocument::applyChanges, which keeps stable ids and is atomic.

namespace prefab_detail {
using Json = nlohmann::json;
using IdMap = std::unordered_map<uint64_t, uint64_t>;

constexpr const char* instanceKey = "prefab_instance";
constexpr const char* entityKey = "prefab_entity";

// Components holding scene organization rather than prefab content: a prefab
// never stores them, an instance keeps its own through every refresh, and they
// are never overrides. Collection ids are per scene.
constexpr std::array<const char*, 2> sceneLocalKeys{"collection_membership", "collection_instance"};

inline bool isSceneLocal(std::string_view key) {
    return std::ranges::find(sceneLocalKeys, key) != sceneLocalKeys.end();
}

// Marks a scene id inside prefab-local data (an instance's override of a
// reference to an entity outside the instance), so it cannot be mistaken for
// a prefab-local id. Stable ids never reach this bit.
constexpr uint64_t sceneReferenceTag = uint64_t{1} << 63u;
// Marks a reference to an entity of a nested instance; see
// prefabNestedReference.
constexpr uint64_t nestedReferenceTag = uint64_t{1} << 62u;

inline bool isSceneReference(uint64_t id) {
    return (id & sceneReferenceTag) != 0;
}

inline bool isNestedReference(uint64_t id) {
    return !isSceneReference(id) && (id & nestedReferenceTag) != 0;
}

inline uint64_t nestedRecordOf(uint64_t reference) {
    return (reference & ~nestedReferenceTag) >> 32u;
}

inline uint64_t nestedLocalOf(uint64_t reference) {
    return reference & 0xFFFFFFFFu;
}

// A reference to an entity of a nested instance that a rebuild resolves once
// the nested instances exist.
struct PendingNestedReference {
    uint64_t entity{0};
    std::string component;
    std::string field;
    uint64_t reference{0};
};

// What becomes of an entity reference whose target the id map lacks.
enum class UnmappedReferences : uint8_t {
    // Scene to prefab-local, for a capture: cleared to none and reported,
    // because a prefab names only its own entities.
    Clear,
    // Scene to prefab-local, for comparing an instance with its prefab:
    // tagged with sceneReferenceTag, as it names an entity outside the
    // instance.
    TagScene,
    // Prefab-local to scene, for rebuilding an instance: tagged scene ids are
    // untagged, references into nested instances are left to pending (none
    // until resolved), and other ids (prefab entities the instance removed)
    // become none where a reference may be none.
    Resolve,
};

// Removes what is not prefab content: the link components and the
// scene-local components.
inline void stripNonContentComponents(Json& entity) {
    Json& components = entity["components"];
    components.erase(instanceKey);
    components.erase(entityKey);
    for (const char* key : sceneLocalKeys) {
        components.erase(key);
    }
}

inline std::string normalizedReference(const std::filesystem::path& reference) {
    return reference.lexically_normal().generic_string();
}

inline PrefabResult fromScene(const SceneDocumentResult& result) {
    return {.success = result.success, .error = result.error};
}

inline Json matrixJson(const glm::mat4& value) {
    Json result = Json::array();
    for (size_t index = 0; index < 16; ++index) {
        result.push_back(glm::value_ptr(value)[index]);
    }
    return result;
}

inline Entity parentOf(const Scene& scene, Entity entity) {
    return scene.registry().get<RelationshipComponent>(entity).parent;
}

// entity and all of its descendants, depth first.
inline std::vector<Entity> subtree(const Scene& scene, Entity entity) {
    std::vector<Entity> result;
    std::vector<Entity> pending{entity};
    while (!pending.empty()) {
        const Entity current = pending.back();
        pending.pop_back();
        result.push_back(current);
        const auto& children = scene.registry().get<RelationshipComponent>(current).children;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return result;
}

inline bool isParentOverride(const PrefabOverride& value) {
    return value.component.empty() && value.field == "/parent";
}

inline bool isRemovedOverride(const PrefabOverride& value) {
    return value.component.empty() && value.field == "/removed";
}

inline std::optional<uint64_t> rootId(const PrefabDocument& document) {
    for (const PrefabEntity& entity : document.entities) {
        if (!entity.parent) {
            return entity.id.value;
        }
    }
    return std::nullopt;
}

// Id remapping (PrefabIdRemap.cpp).
void remapIds(Json& entity, const IdMap& map, const ComponentTypeRegistry& types, UnmappedReferences unmapped,
              std::vector<std::string>* diagnostics = nullptr, std::vector<PendingNestedReference>* pending = nullptr);

std::vector<std::pair<Entity, PrefabEntityId>> mappedMembers(const Scene& scene, Entity root,
                                                             std::vector<Entity>* added = nullptr,
                                                             std::vector<Entity>* nestedRoots = nullptr);

void mapNestedReferences(const Scene& scene, Entity nestedRoot, uint64_t record, IdMap& sceneToReference);

// How an instance's scene entities are named in its prefab's reference space.
struct InstanceIds {
    // Mapped members to their prefab-local id, and the prefab entities of
    // nested record instances to nested references.
    IdMap sceneToLocal;
    std::vector<Entity> mapped;
    // Scene ids of members added in the scene.
    std::unordered_set<uint64_t> added;
};

InstanceIds instanceIds(const Scene& scene, Entity root);

// Staging serialization and applying overrides (PrefabOverrides.cpp).
PrefabResult serializeEntities(const Scene& scene, std::span<const Entity> entities, const AssetManager& assets,
                               const SceneDocumentOptions& options, std::map<uint64_t, Json>& output);

void applyOverrides(std::map<uint64_t, Json>& entities, std::span<const PrefabOverride> overrides, uint64_t rootLocal,
                    std::vector<std::string>& diagnostics);

bool resolveSceneParents(std::map<uint64_t, Json>& entities, const PrefabDocument& document,
                         const std::unordered_set<uint64_t>& stays, std::vector<std::string>& diagnostics);

// Destroys every entity whose stable id is firstCreated or higher: what a
// failed instantiation created, since ids only grow.
void destroyEntitiesFrom(Scene& scene, uint64_t firstCreated);

// Nested instances (PrefabNesting.cpp).
std::unordered_set<uint64_t> removedRecords(std::span<const PrefabOverride> overrides);

std::vector<PrefabOverride> capturedRecordOverrides(std::vector<PrefabOverride> overrides, const IdMap& sceneToLocal,
                                                    const ComponentTypeRegistry& types, const std::string& name,
                                                    std::vector<std::string>& diagnostics);

std::vector<PrefabOverride> sceneRecordOverrides(std::span<const PrefabOverride> overrides, const IdMap& localToScene,
                                                 const ComponentTypeRegistry& types);

PrefabResult mergeNestedOverrides(const PrefabSystem& prefabs, const Scene& scene, Entity nested,
                                  std::vector<PrefabOverride>& overrides);

void resolveNestedReferences(Scene& scene, Entity root, std::span<const PendingNestedReference> pending,
                             AssetManager& assets, const SceneDocumentOptions& options,
                             std::vector<std::string>& diagnostics);
} // namespace prefab_detail

// One prefab file as builds use it, read once per file version
// (PrefabCache.cpp).
struct PrefabSystem::CachedPrefab {
    // The file version it was read from.
    std::filesystem::file_time_type writeTime{};
    std::uintmax_t size{0};
    // When the file was last compared with that version. Mutable: refreshing
    // it does not change the cached content.
    mutable std::chrono::steady_clock::time_point checked{};
    // What loading the file returned, diagnostics included. On failure
    // nothing below is set.
    PrefabResult loaded;
    PrefabDocument document;
    uint64_t rootLocal{0};
    // Each entity in the scene document entity form, with prefab-local ids
    // and without non-content components, keyed by prefab-local id.
    std::map<uint64_t, prefab_detail::Json> entities;
    // Why an entity could not be put in that form; builds fail with it.
    std::string conversionError;
    // Whether every entity's components read through the component codecs
    // (checked once, in a staging scene). Overrides can still make an
    // instance's components invalid.
    bool contentValid{false};
};
