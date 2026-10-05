// Prefab id remapping: rewriting entity documents between scene ids,
// prefab-local ids, and nested references.
#include "PrefabSystemInternal.hpp"

namespace prefab_detail {
// Rewrites the ids an entity document object holds (its id, its parent, and
// every entity reference its components hold, see remapEntityReferences)
// through map. Ids the map lacks keep their id and parent; references are
// handled as unmapped says.
void remapIds(Json& entity, const IdMap& map, const ComponentTypeRegistry& types, UnmappedReferences unmapped,
              std::vector<std::string>* diagnostics, std::vector<PendingNestedReference>* pending) {
    const auto remap = [&map](Json& value) {
        if (!value.is_number_unsigned()) {
            return;
        }
        if (const auto found = map.find(value.get<uint64_t>()); found != map.end()) {
            value = found->second;
        }
    };
    remap(entity["id"]);
    remap(entity["parent"]);
    // Read only for a diagnostic, so the common case copies no string.
    const auto name = [&entity] { return entity.value("name", std::string{}); };
    remapEntityReferences(
        entity["components"], types, [&](uint64_t id, const EntityReferenceSite& site) -> std::optional<uint64_t> {
            if (unmapped == UnmappedReferences::Resolve && isSceneReference(id)) {
                return id & ~sceneReferenceTag;
            }
            if (const auto found = map.find(id); found != map.end()) {
                return found->second;
            }
            if (unmapped == UnmappedReferences::Resolve && isNestedReference(id) && pending != nullptr) {
                pending->push_back({.entity = entity.value("id", uint64_t{0}),
                                    .component = std::string(site.component),
                                    .field = std::string(site.field),
                                    .reference = id});
                return site.nullable ? std::optional<uint64_t>(0) : std::nullopt;
            }
            if (unmapped == UnmappedReferences::TagScene) {
                return id | sceneReferenceTag;
            }
            if (!site.nullable) {
                return std::nullopt;
            }
            if (unmapped == UnmappedReferences::Resolve) {
                return uint64_t{0};
            }
            if (diagnostics != nullptr) {
                diagnostics->push_back("'" + name() + "' " + std::string(site.component) + std::string(site.field) +
                                       " referenced an entity outside the prefab; the prefab "
                                       "stores no reference there");
            }
            return uint64_t{0};
        });
}

// The members of an instance that stand for prefab entities: the first member
// claiming each local id. A second member claiming one (a copy pasted into the
// instance) counts as added.
std::vector<std::pair<Entity, PrefabEntityId>>
mappedMembers(const Scene& scene, Entity root, std::vector<Entity>* added, std::vector<Entity>* nestedRoots) {
    std::vector<std::pair<Entity, PrefabEntityId>> mapped;
    std::unordered_set<uint64_t> locals;
    for (const Entity member : prefabInstanceMembers(scene, root, nestedRoots)) {
        const auto* marker = scene.registry().try_get<PrefabEntityComponent>(member);
        if (marker != nullptr && marker->entity && locals.insert(marker->entity.value).second) {
            mapped.emplace_back(member, marker->entity);
        } else if (added != nullptr) {
            added->push_back(member);
        }
    }
    return mapped;
}

// Maps the scene ids of the nested instance's prefab entities to nested
// references for record.
void mapNestedReferences(const Scene& scene, Entity nestedRoot, uint64_t record, IdMap& sceneToReference) {
    for (const auto& [member, local] : mappedMembers(scene, nestedRoot)) {
        if (const uint64_t reference = prefabNestedReference(record, local); reference != 0) {
            sceneToReference.emplace(scene.id(member).value, reference);
        }
    }
}

InstanceIds instanceIds(const Scene& scene, Entity root) {
    InstanceIds ids;
    std::vector<Entity> added;
    std::vector<Entity> nestedRoots;
    for (const auto& [member, local] : mappedMembers(scene, root, &added, &nestedRoots)) {
        ids.mapped.push_back(member);
        ids.sceneToLocal.emplace(scene.id(member).value, local.value);
    }
    for (const Entity member : added) {
        ids.added.insert(scene.id(member).value);
    }
    std::unordered_set<uint64_t> records;
    for (const Entity nested : nestedRoots) {
        const uint64_t record = scene.registry().get<PrefabInstanceComponent>(nested).nestedId;
        if (record != 0 && records.insert(record).second) {
            mapNestedReferences(scene, nested, record, ids.sceneToLocal);
        }
    }
    return ids;
}
} // namespace prefab_detail
