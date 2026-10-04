// Nested prefab instances: their records' overrides, keeping nested instances
// through a refresh, resolving references into them, and the nested graph check.
#include "PrefabSystemInternal.hpp"

#include <functional>

using namespace prefab_detail;

namespace {
bool sameTarget(const PrefabOverride& left, const PrefabOverride& right) {
    return left.entity == right.entity && left.nested == right.nested && left.component == right.component &&
           left.field == right.field;
}

// Rewrites the entity references inside a component override's value through
// mapping (as remapEntityReferences does for a whole entity).
void remapOverrideReferences(PrefabOverride& value, const ComponentTypeRegistry& types,
                             const EntityReferenceMapping& mapping) {
    if (value.component.empty() || value.nested != 0) {
        return;
    }
    Json parsed = Json::parse(value.jsonValue, nullptr, false);
    if (parsed.is_discarded() || parsed.is_null()) {
        return;
    }
    try {
        Json components = {{value.component, Json::object()}};
        Json& component = components[value.component];
        if (value.field.empty()) {
            component = std::move(parsed);
        } else {
            component[Json::json_pointer(value.field)] = std::move(parsed);
        }
        remapEntityReferences(components, types, mapping);
        value.jsonValue = (value.field.empty() ? component : component.at(Json::json_pointer(value.field))).dump();
    } catch (const Json::exception&) {
        // A pointer that cannot address the value holds no reference.
    }
}
} // namespace

namespace prefab_detail {
// The nested records the overrides remove.
std::unordered_set<uint64_t> removedRecords(std::span<const PrefabOverride> overrides) {
    std::unordered_set<uint64_t> removed;
    for (const PrefabOverride& value : overrides) {
        if (value.nested != 0 && isRemovedOverride(value) && value.jsonValue == "true") {
            removed.insert(value.nested);
        }
    }
    return removed;
}

// A nested record's overrides as written into a prefab file: references to
// the captured entities (sceneToLocal, scene ids tagged with
// sceneReferenceTag in the instance's overrides) become their prefab-local ids
// with the tag, and other scene references are dropped, so the file holds no
// scene ids.
std::vector<PrefabOverride> capturedRecordOverrides(std::vector<PrefabOverride> overrides, const IdMap& sceneToLocal,
                                                    const ComponentTypeRegistry& types, const std::string& name,
                                                    std::vector<std::string>& diagnostics) {
    std::vector<PrefabOverride> kept;
    for (PrefabOverride& value : overrides) {
        if (isParentOverride(value)) {
            const Json parent = Json::parse(value.jsonValue, nullptr, false);
            if (parent.is_number_unsigned() && isSceneReference(parent.get<uint64_t>())) {
                diagnostics.push_back("Nested prefab '" + name +
                                      "': an entity moved under an object added in the scene keeps its prefab "
                                      "parent in the prefab");
                continue;
            }
        }
        bool drop = false;
        remapOverrideReferences(
            value, types, [&](uint64_t id, const EntityReferenceSite& site) -> std::optional<uint64_t> {
                if (!isSceneReference(id)) {
                    return std::nullopt;
                }
                if (const auto found = sceneToLocal.find(id & ~sceneReferenceTag);
                    found != sceneToLocal.end() && !isNestedReference(found->second)) {
                    return found->second | sceneReferenceTag;
                }
                diagnostics.push_back("Nested prefab '" + name + "' " + std::string(site.component) +
                                      std::string(site.field) +
                                      " referenced an entity outside the prefab; the prefab stores no reference there");
                drop = drop || !site.nullable;
                return uint64_t{0};
            });
        if (!drop) {
            kept.push_back(std::move(value));
        }
    }
    return kept;
}

// A nested record's overrides for building its instance inside the containing
// instance: references to the containing prefab's entities (tagged
// prefab-local ids) become those entities' scene ids, tagged as references
// outside the nested instance.
std::vector<PrefabOverride> sceneRecordOverrides(std::span<const PrefabOverride> overrides, const IdMap& localToScene,
                                                 const ComponentTypeRegistry& types) {
    std::vector<PrefabOverride> result;
    for (PrefabOverride value : overrides) {
        bool drop = false;
        remapOverrideReferences(
            value, types, [&](uint64_t id, const EntityReferenceSite& site) -> std::optional<uint64_t> {
                if (!isSceneReference(id)) {
                    return std::nullopt;
                }
                if (const auto found = localToScene.find(id & ~sceneReferenceTag); found != localToScene.end()) {
                    return found->second | sceneReferenceTag;
                }
                drop = drop || !site.nullable;
                return uint64_t{0};
            });
        if (!drop) {
            result.push_back(std::move(value));
        }
    }
    return result;
}

// The overrides to refresh a kept nested instance with when its containing
// instance keeps its own changes: the containing prefab's (possibly changed)
// record, plus what the nested instance changed in the scene since it was last
// synchronized with that record. Its persisted overrides are the record's
// values as of that synchronization, so live overrides equal to them came from
// the old record and give way to the new one.
PrefabResult mergeNestedOverrides(const PrefabSystem& prefabs, const Scene& scene, Entity nested,
                                  std::vector<PrefabOverride>& overrides) {
    const auto& instance = scene.registry().get<PrefabInstanceComponent>(nested);
    std::vector<PrefabOverride> live;
    if (PrefabResult compared = prefabs.overrides(scene, nested, live); !compared) {
        return compared;
    }
    const auto upsert = [&overrides](PrefabOverride value) {
        const auto found = std::ranges::find_if(
            overrides, [&value](const PrefabOverride& existing) { return sameTarget(existing, value); });
        if (found != overrides.end()) {
            *found = std::move(value);
        } else {
            overrides.push_back(std::move(value));
        }
    };
    for (const PrefabOverride& value : live) {
        // A removal is only ever recorded on the instance itself (deleting an
        // entity in the scene), so it always stays.
        if (isRemovedOverride(value) || std::ranges::find(instance.overrides, value) == instance.overrides.end()) {
            upsert(value);
        }
    }
    // The nested root's placement is never a live override; keep it when it
    // moved since the last synchronization.
    if (const auto* rootMarker = scene.registry().try_get<PrefabEntityComponent>(nested)) {
        const PrefabOverride placement{.entity = rootMarker->entity,
                                       .field = "/local_transform",
                                       .jsonValue =
                                           matrixJson(scene.registry().get<TransformComponent>(nested).local).dump()};
        const auto persisted = std::ranges::find_if(
            instance.overrides, [&placement](const PrefabOverride& value) { return sameTarget(value, placement); });
        if (persisted == instance.overrides.end() || persisted->jsonValue != placement.jsonValue) {
            upsert(placement);
        }
    }
    return {.success = true};
}

// Points the references a rebuild left pending (see remapIds) at the entities
// of root's nested instances, now that those exist.
void resolveNestedReferences(Scene& scene, Entity root, std::span<const PendingNestedReference> pending,
                             AssetManager& assets, const SceneDocumentOptions& options,
                             std::vector<std::string>& diagnostics) {
    if (pending.empty()) {
        return;
    }
    std::vector<Entity> nestedRoots;
    prefabInstanceMembers(scene, root, &nestedRoots);
    std::unordered_map<uint64_t, IdMap> recordEntities;
    for (const Entity nested : nestedRoots) {
        const uint64_t record = scene.registry().get<PrefabInstanceComponent>(nested).nestedId;
        if (record == 0 || recordEntities.contains(record)) {
            continue;
        }
        IdMap& locals = recordEntities[record];
        for (const auto& [member, local] : mappedMembers(scene, nested)) {
            locals.emplace(local.value, scene.id(member).value);
        }
    }
    const ComponentTypeRegistry& types = componentTypesFor(options);
    for (const PendingNestedReference& reference : pending) {
        const Entity entity = scene.findEntity({reference.entity});
        const ComponentType* type = types.find(reference.component);
        if (entity == entt::null || type == nullptr || !type->has(scene, entity)) {
            continue;
        }
        uint64_t target = 0;
        if (const auto record = recordEntities.find(nestedRecordOf(reference.reference));
            record != recordEntities.end()) {
            if (const auto found = record->second.find(nestedLocalOf(reference.reference));
                found != record->second.end()) {
                target = found->second;
            }
        }
        const std::string& name = scene.registry().get<NameComponent>(entity).name;
        if (target == 0) {
            diagnostics.push_back("'" + name + "' " + reference.component + reference.field +
                                  " references an entity of a nested prefab the instance no longer has");
            continue;
        }
        Json component = Json::object();
        if (!type->save(scene, entity, {.assets = assets, .options = options}, component)) {
            continue;
        }
        try {
            component[Json::json_pointer(reference.field)] = target;
        } catch (const Json::exception&) {
            continue;
        }
        if (SceneDocumentResult loaded = type->load(component, scene, entity, {.assets = assets, .options = options});
            !loaded) {
            diagnostics.push_back("'" + name + "' " + reference.component + ": " + loaded.error);
        }
    }
}
} // namespace prefab_detail

PrefabResult PrefabSystem::checkNestedGraph(const std::filesystem::path& reference,
                                            const PrefabDocument& document) const {
    std::vector<std::string> stack{normalizedReference(reference)};
    std::function<PrefabResult(const std::filesystem::path&, const PrefabDocument&)> visit =
        [&](const std::filesystem::path& containing, const PrefabDocument& current) -> PrefabResult {
        for (const NestedPrefab& record : current.nested) {
            const std::filesystem::path child = (containing.parent_path() / record.source).lexically_normal();
            const std::string key = normalizedReference(child);
            if (std::ranges::find(stack, key) != stack.end()) {
                return {.error = "Saving " + normalizedReference(reference) +
                                 " would make a nested prefab cycle through " + key};
            }
            PrefabDocument nested;
            if (PrefabResult loaded = load(child, nested); !loaded) {
                return loaded;
            }
            stack.push_back(key);
            if (PrefabResult result = visit(child, nested); !result) {
                return result;
            }
            stack.pop_back();
        }
        return {.success = true};
    };
    return visit(reference, document);
}
