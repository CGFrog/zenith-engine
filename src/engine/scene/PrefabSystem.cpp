// PrefabSystem: instantiating, refreshing, and capturing prefab instances.
// Comparison and overrides are in PrefabOverrides.cpp, nested instances in
// PrefabNesting.cpp, and id remapping in PrefabIdRemap.cpp.
#include "PrefabSystemInternal.hpp"

#include <functional>

using namespace prefab_detail;

namespace {
// Built-in custom-codec fields naming the instance root of a model's node
// entities; a model whose nodes a prefab lists names its entity in one.
struct ModelRootField {
    const char* component;
    const char* field;
};

constexpr std::array<ModelRootField, 2> modelRootFields{{
    {"model_node", "instance_root"},
    {"skin", "instance_root"},
}};

// The scene-local component objects entity holds now, keyed by component key.
PrefabResult sceneLocalComponents(const Scene& scene, Entity entity, const AssetManager& assets,
                                  const SceneDocumentOptions& options, Json& output) {
    output = Json::object();
    const ComponentTypeRegistry& types = componentTypesFor(options);
    for (const char* key : sceneLocalKeys) {
        const ComponentType* type = types.find(key);
        if (type == nullptr || !type->has(scene, entity)) {
            continue;
        }
        Json value = Json::object();
        if (SceneDocumentResult saved = type->save(scene, entity, {.assets = assets, .options = options}, value);
            !saved) {
            return {.error = saved.error};
        }
        output[key] = std::move(value);
    }
    return {.success = true};
}

bool hasModelNodes(const Scene& scene, Entity entity) {
    const SceneEntityId id = scene.id(entity);
    for (const Entity descendant : subtree(scene, entity)) {
        const auto* node = scene.registry().try_get<ModelNodeComponent>(descendant);
        if (node != nullptr && node->instanceRoot == id) {
            return true;
        }
    }
    return false;
}

// A prefab entity holding a model component whose node entities the prefab
// does not list (version 1 `modelAsset` prefabs, hand-written prefabs) gets the
// model's node hierarchy instantiated under it.
//
// The node entities get prefab-local ids above the document's, in node order,
// so every instance numbers them alike: a later apply writes them into the
// prefab and other instances then match them by id instead of duplicating
// them.
PrefabResult expandModels(const std::map<uint64_t, Json>& entities, const IdMap& localToScene,
                          const AssetManager& assets, Scene& scene) {
    uint64_t nextLocal = entities.empty() ? 1 : entities.rbegin()->first + 1;
    std::unordered_set<uint64_t> listedRoots;
    for (const auto& [local, json] : entities) {
        for (const ModelRootField& reference : modelRootFields) {
            const Json& components = json.at("components");
            if (const auto component = components.find(reference.component);
                component != components.end() && component->contains(reference.field)) {
                listedRoots.insert(component->at(reference.field).get<uint64_t>());
            }
        }
    }
    for (const auto& [local, json] : entities) {
        if (!json.at("components").contains("model") || listedRoots.contains(local)) {
            continue;
        }
        const Entity entity = scene.findEntity({localToScene.at(local)});
        auto* model = scene.registry().try_get<ModelComponent>(entity);
        if (model == nullptr || hasModelNodes(scene, entity)) {
            continue;
        }
        const ModelHandle handle = model->model;
        scene.registry().remove<ModelComponent>(entity);
        if (SceneResult result = scene.instantiateModel(entity, handle, assets); !result) {
            return {.error = "Could not instantiate prefab model: " + result.error};
        }
        std::vector<std::pair<uint32_t, Entity>> nodes;
        for (const Entity descendant : subtree(scene, entity)) {
            const auto* node = scene.registry().try_get<ModelNodeComponent>(descendant);
            if (node != nullptr && node->instanceRoot == scene.id(entity)) {
                nodes.emplace_back(node->nodeIndex, descendant);
            }
        }
        std::ranges::sort(nodes, {}, &std::pair<uint32_t, Entity>::first);
        for (const auto& [index, node] : nodes) {
            scene.registry().emplace_or_replace<PrefabEntityComponent>(node, PrefabEntityId{nextLocal++});
        }
    }
    return {.success = true};
}
} // namespace

PrefabInstanceResult PrefabSystem::transact(Scene& scene, const std::function<PrefabInstanceResult()>& change) {
    // The staging validation catches nearly every failure; this restores the
    // scene when a nested instance fails after its container was rebuilt.
    std::string before;
    const bool restorable = SceneDocument::serialize(scene, m_assets, m_options, before).success;
    PrefabInstanceResult result = change();
    if (result || !restorable) {
        return result;
    }
    std::string after;
    SceneChanges changes;
    if (!SceneDocument::serialize(scene, m_assets, m_options, after) || !SceneDocument::diff(before, after, changes) ||
        !SceneDocument::applyChanges(changes, SceneChangeDirection::Backward, m_assets, m_options, scene)) {
        result.error += " (the scene could not be restored)";
    }
    return result;
}

PrefabSystem::PrefabSystem(AssetManager& assets, SceneDocumentOptions options)
    : m_assets(assets)
    , m_options(std::move(options)) {
    m_options.assetRoot = std::filesystem::absolute(m_options.assetRoot).lexically_normal();
}

PrefabResult PrefabSystem::resolve(const std::filesystem::path& reference, std::filesystem::path& path) const {
    if (reference.empty() || reference.is_absolute()) {
        return {.error = "Prefab references must be project-relative: " + reference.generic_string()};
    }
    path = (m_options.assetRoot / reference).lexically_normal();
    const std::filesystem::path relative = path.lexically_relative(m_options.assetRoot);
    if (relative.empty() || relative == "." || *relative.begin() == "..") {
        return {.error = "Prefab reference escapes the project asset root: " + reference.generic_string()};
    }
    return {.success = true};
}

PrefabResult PrefabSystem::load(const std::filesystem::path& reference, PrefabDocument& document) const {
    std::filesystem::path path;
    if (PrefabResult result = resolve(reference, path); !result) {
        return result;
    }
    return loadPrefab(path, document);
}

PrefabInstanceResult PrefabSystem::instantiate(const std::filesystem::path& reference, Scene& scene, Entity parent,
                                               std::span<const PrefabOverride> overrides) {
    // Resolve the complete nested graph in isolation first, so a missing
    // source, cycle, or invalid component cannot partially change the scene.
    Scene validation;
    std::vector<std::filesystem::path> validationStack;
    if (PrefabInstanceResult checked = build(reference, validation, entt::null, entt::null, overrides, 0,
                                             PrefabRefreshMode::RemoveAddedObjects, validationStack);
        !checked) {
        return checked;
    }
    return transact(scene, [&] {
        std::vector<std::filesystem::path> stack;
        return build(reference, scene, entt::null, parent, overrides, 0, PrefabRefreshMode::RemoveAddedObjects, stack);
    });
}

PrefabInstanceResult PrefabSystem::refresh(Scene& scene, Entity root, std::span<const PrefabOverride> overrides,
                                           PrefabRefreshMode mode) {
    const auto* instance = root == entt::null || !scene.registry().valid(root)
                               ? nullptr
                               : scene.registry().try_get<PrefabInstanceComponent>(root);
    if (instance == nullptr) {
        return {.error = "The entity is not a prefab instance root"};
    }
    // Copied: the rebuild replaces the component the arguments may point into.
    const std::filesystem::path reference = instance->prefab;
    const uint64_t nestedId = instance->nestedId;
    const std::vector<PrefabOverride> values(overrides.begin(), overrides.end());
    Scene validation;
    std::vector<std::filesystem::path> validationStack;
    if (PrefabInstanceResult checked = build(reference, validation, entt::null, entt::null, values, nestedId,
                                             PrefabRefreshMode::RemoveAddedObjects, validationStack);
        !checked) {
        return checked;
    }
    return transact(scene, [&] {
        std::vector<std::filesystem::path> stack;
        return build(reference, scene, root, entt::null, values, nestedId, mode, stack);
    });
}

PrefabInstanceResult PrefabSystem::refresh(Scene& scene, Entity root, PrefabRefreshMode mode) {
    const auto* instance = root == entt::null || !scene.registry().valid(root)
                               ? nullptr
                               : scene.registry().try_get<PrefabInstanceComponent>(root);
    if (instance == nullptr) {
        return {.error = "The entity is not a prefab instance root"};
    }
    const std::vector<PrefabOverride> persisted = instance->overrides;
    return refresh(scene, root, persisted, mode);
}

PrefabInstanceResult PrefabSystem::build(const std::filesystem::path& reference, Scene& scene, Entity existingRoot,
                                         Entity parent, std::span<const PrefabOverride> overrides, uint64_t nestedId,
                                         PrefabRefreshMode mode, std::vector<std::filesystem::path>& stack) {
    std::filesystem::path path;
    if (PrefabResult resolved = resolve(reference, path); !resolved) {
        return {.error = resolved.error};
    }
    if (std::ranges::find(stack, path) != stack.end()) {
        return {.error = "Nested prefab cycle detected at: " + normalizedReference(reference)};
    }
    PrefabDocument document;
    PrefabResult loaded = loadPrefab(path, document);
    if (!loaded) {
        return {.error = loaded.error};
    }
    PrefabInstanceResult result{.success = true, .diagnostics = std::move(loaded.diagnostics)};
    const uint64_t rootLocal = *rootId(document);

    std::map<uint64_t, Json> entities;
    for (const PrefabEntity& entity : document.entities) {
        Json& json = entities[entity.id.value];
        if (PrefabResult encoded = prefabEntityToJson(entity, json); !encoded) {
            return {.error = encoded.error};
        }
        // Older prefabs may hold collection ids of the scene they came from.
        stripNonContentComponents(json);
    }
    applyOverrides(entities, overrides, rootLocal, result.diagnostics);
    const bool rootTransformOverridden = std::ranges::any_of(overrides, [rootLocal](const PrefabOverride& value) {
        return value.entity.value == rootLocal && value.component.empty() && value.field == "/local_transform";
    });

    const std::unordered_set<uint64_t> removedNested = removedRecords(overrides);

    // Which existing entities stay, and which scene ids the prefab's entities
    // already have.
    IdMap localToScene;
    std::map<uint64_t, Entity> keptNested;
    std::unordered_set<uint64_t> stays;
    std::vector<Entity> nestedRoots;
    std::vector<Entity> members;
    if (existingRoot != entt::null) {
        const auto* rootMarker = scene.registry().try_get<PrefabEntityComponent>(existingRoot);
        if (rootMarker == nullptr || rootMarker->entity.value != rootLocal) {
            return {.error = "The instance root no longer matches the prefab's root entity; unpack the instance "
                             "and instantiate the prefab again"};
        }
        members = prefabInstanceMembers(scene, existingRoot, &nestedRoots);
    }
    const auto findStays = [&] {
        localToScene.clear();
        keptNested.clear();
        stays.clear();
        if (existingRoot == entt::null) {
            return;
        }
        for (const Entity member : members) {
            const uint64_t sceneId = scene.id(member).value;
            const auto* marker = scene.registry().try_get<PrefabEntityComponent>(member);
            if (marker != nullptr && entities.contains(marker->entity.value) &&
                !localToScene.contains(marker->entity.value)) {
                localToScene.emplace(marker->entity.value, sceneId);
                stays.insert(sceneId);
            } else if (mode == PrefabRefreshMode::KeepAddedObjects && member != existingRoot &&
                       (marker == nullptr || entities.contains(marker->entity.value)) &&
                       stays.contains(scene.id(parentOf(scene, member)).value)) {
                // Added in the scene, or a copy of a prefab entity. Entities
                // whose prefab entity was deleted are removed.
                stays.insert(sceneId);
            }
        }
        // A nested instance stands for a record only while it instantiates the
        // record's prefab.
        std::unordered_map<uint64_t, std::string> recordSources;
        for (const NestedPrefab& record : document.nested) {
            if (!removedNested.contains(record.id)) {
                recordSources.emplace(record.id, normalizedReference(reference.parent_path() / record.source));
            }
        }
        for (const Entity nested : nestedRoots) {
            const auto& nestedInstance = scene.registry().get<PrefabInstanceComponent>(nested);
            const uint64_t id = nestedInstance.nestedId;
            const bool parentStays = stays.contains(scene.id(parentOf(scene, nested)).value);
            const auto source = recordSources.find(id);
            const bool isRecord = source != recordSources.end() &&
                                  source->second == normalizedReference(nestedInstance.prefab) &&
                                  !keptNested.contains(id);
            // An instance of a removed record goes (an apply removes the
            // records it deleted from the other instances this way).
            const bool recordGone = removedNested.contains(id);
            if (parentStays && !recordGone && (isRecord || mode == PrefabRefreshMode::KeepAddedObjects)) {
                if (isRecord) {
                    keptNested.emplace(id, nested);
                }
                for (const Entity entity : subtree(scene, nested)) {
                    stays.insert(scene.id(entity).value);
                }
            }
        }
    };
    // Moving entities back under their prefab parents can remove others, so
    // repeat until nothing changes.
    do {
        findStays();
    } while (resolveSceneParents(entities, document, stays, result.diagnostics));
    SceneChanges changes;
    if (existingRoot != entt::null) {
        for (const Entity entity : subtree(scene, existingRoot)) {
            if (!stays.contains(scene.id(entity).value)) {
                changes.entities.push_back({.id = scene.id(entity)});
            }
        }
    }

    // Rebuilt entities keep their scene organization (collection links).
    std::unordered_map<uint64_t, Json> keptSceneLocal;
    for (const auto& [local, sceneId] : localToScene) {
        if (PrefabResult saved =
                sceneLocalComponents(scene, scene.findEntity({sceneId}), m_assets, m_options, keptSceneLocal[local]);
            !saved) {
            return {.error = saved.error};
        }
    }

    for (const auto& [local, json] : entities) {
        if (!localToScene.contains(local)) {
            localToScene.emplace(local, scene.reserveEntityId().value);
        }
    }

    const ComponentTypeRegistry& types = componentTypesFor(m_options);
    std::vector<PendingNestedReference> pending;
    for (const auto& [local, json] : entities) {
        Json target = json;
        remapIds(target, localToScene, types, UnmappedReferences::Resolve, nullptr, &pending);
        // Under an entity added in the scene (resolveSceneParents kept it).
        if (Json& targetParent = target["parent"];
            targetParent.is_number_unsigned() && isSceneReference(targetParent.get<uint64_t>())) {
            targetParent = targetParent.get<uint64_t>() & ~sceneReferenceTag;
        }
        if (const auto kept = keptSceneLocal.find(local); kept != keptSceneLocal.end()) {
            target["components"].update(kept->second);
        }
        target["components"][entityKey] = {{"entity", local}};
        if (local == rootLocal) {
            const Entity rootParent = existingRoot != entt::null ? parentOf(scene, existingRoot) : parent;
            target["parent"] = rootParent == entt::null ? Json(nullptr) : Json(scene.id(rootParent).value);
            if (existingRoot != entt::null && !rootTransformOverridden) {
                target["local_transform"] = matrixJson(scene.registry().get<TransformComponent>(existingRoot).local);
            }
            Json persisted = Json::array();
            for (const PrefabOverride& value : overrides) {
                persisted.push_back(prefabOverrideToJson(value));
            }
            target["components"][instanceKey] = {{"prefab", {{"path", normalizedReference(reference)}}},
                                                 {"overrides", std::move(persisted)}};
            if (nestedId != 0) {
                target["components"][instanceKey]["nested_id"] = nestedId;
            }
        }
        changes.entities.push_back({.id = {localToScene.at(local)}, .after = target.dump()});
    }
    if (SceneDocumentResult applied =
            SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, m_assets, m_options, scene);
        !applied) {
        return {.error = normalizedReference(reference) + ": " + applied.error};
    }
    if (PrefabResult expanded = expandModels(entities, localToScene, m_assets, scene); !expanded) {
        return {.error = expanded.error};
    }
    const Entity root = scene.findEntity({localToScene.at(rootLocal)});

    stack.push_back(path);
    for (const NestedPrefab& record : document.nested) {
        if (removedNested.contains(record.id)) {
            continue;
        }
        const std::filesystem::path nestedReference = (reference.parent_path() / record.source).lexically_normal();
        const auto kept = keptNested.find(record.id);
        std::vector<PrefabOverride> nestedOverrides = sceneRecordOverrides(record.overrides, localToScene, types);
        PrefabInstanceResult child;
        if (kept != keptNested.end()) {
            if (mode == PrefabRefreshMode::KeepAddedObjects) {
                if (PrefabResult merged = mergeNestedOverrides(*this, scene, kept->second, nestedOverrides); !merged) {
                    stack.pop_back();
                    return {.error = merged.error};
                }
            }
            child = build(nestedReference, scene, kept->second, entt::null, nestedOverrides, record.id, mode, stack);
        } else {
            const uint64_t parentLocal = record.parent ? record.parent->value : rootLocal;
            if (!entities.contains(parentLocal)) {
                // The instance removed the entity the record is parented to.
                continue;
            }
            const Entity nestedParent = scene.findEntity({localToScene.at(parentLocal)});
            child = build(nestedReference, scene, entt::null, nestedParent, nestedOverrides, record.id, mode, stack);
        }
        if (!child) {
            stack.pop_back();
            return child;
        }
        result.diagnostics.insert(result.diagnostics.end(), child.diagnostics.begin(), child.diagnostics.end());
    }
    stack.pop_back();
    resolveNestedReferences(scene, root, pending, m_assets, m_options, result.diagnostics);

    scene.updateWorldTransforms();
    result.root = root;
    result.entities = subtree(scene, root);
    return result;
}

PrefabResult PrefabSystem::capture(const Scene& scene, Entity root, const std::filesystem::path& reference,
                                   PrefabCapture& output) const {
    std::filesystem::path path;
    if (PrefabResult resolved = resolve(reference, path); !resolved) {
        return resolved;
    }
    std::vector<Entity> nestedRoots;
    const std::vector<Entity> members = prefabInstanceMembers(scene, root, &nestedRoots);
    const auto* instance = scene.registry().try_get<PrefabInstanceComponent>(root);
    const bool applying =
        instance != nullptr && normalizedReference(instance->prefab) == normalizedReference(reference);

    PrefabCapture captured;
    IdMap sceneToLocal;
    std::unordered_set<uint64_t> usedLocal;
    if (applying) {
        for (const Entity member : members) {
            const auto* marker = scene.registry().try_get<PrefabEntityComponent>(member);
            if (marker != nullptr && marker->entity && usedLocal.insert(marker->entity.value).second) {
                sceneToLocal.emplace(scene.id(member).value, marker->entity.value);
            }
        }
    }
    // An apply never reuses the ids of entities or nested records the prefab
    // had, so other instances cannot mistake a new entity for a deleted one.
    PrefabDocument previous;
    const bool hasPrevious = applying && load(reference, previous).success;
    uint64_t nextLocal = usedLocal.empty() ? 1 : *std::ranges::max_element(usedLocal) + 1;
    uint64_t nextNested = 1;
    if (hasPrevious) {
        for (const PrefabEntity& entity : previous.entities) {
            nextLocal = std::max(nextLocal, entity.id.value + 1);
        }
        for (const NestedPrefab& record : previous.nested) {
            nextNested = std::max(nextNested, record.id + 1);
        }
    }
    for (const Entity member : members) {
        if (!sceneToLocal.contains(scene.id(member).value)) {
            sceneToLocal.emplace(scene.id(member).value, nextLocal++);
        }
    }

    // An apply keeps the prefab's own root placement; a new prefab starts from
    // the captured root's.
    std::optional<glm::mat4> rootTransform;
    if (hasPrevious) {
        for (const PrefabEntity& entity : previous.entities) {
            if (!entity.parent) {
                rootTransform = entity.localTransform;
            }
        }
    }
    // The display name is the prefab's own; renaming an instance root does
    // not rename the prefab.
    captured.document.displayName = applying ? (hasPrevious ? previous.displayName : std::string{})
                                             : scene.registry().get<NameComponent>(root).name;

    // Nested records keep their ids on an apply and are numbered after the
    // prefab's records otherwise; references into them name the record.
    std::unordered_set<uint64_t> usedNested;
    std::vector<std::pair<Entity, uint64_t>> nestedIds;
    for (const Entity nested : nestedRoots) {
        const uint64_t id = scene.registry().get<PrefabInstanceComponent>(nested).nestedId;
        nestedIds.emplace_back(nested, applying && id != 0 && usedNested.insert(id).second ? id : 0);
    }
    if (!usedNested.empty()) {
        nextNested = std::max(nextNested, *std::ranges::max_element(usedNested) + 1);
    }
    IdMap referenceIds = sceneToLocal;
    for (auto& [nested, id] : nestedIds) {
        if (id == 0) {
            id = nextNested++;
        }
        mapNestedReferences(scene, nested, id, referenceIds);
    }

    const ComponentTypeRegistry& types = componentTypesFor(m_options);
    std::map<uint64_t, Json> serialized;
    if (PrefabResult result = serializeEntities(scene, members, m_assets, m_options, serialized); !result) {
        return result;
    }
    std::vector<std::string> diagnostics;
    for (const Entity member : members) {
        Json& json = serialized.at(scene.id(member).value);
        remapIds(json, referenceIds, types, UnmappedReferences::Clear, &diagnostics);
        stripNonContentComponents(json);
        PrefabEntity entity;
        if (PrefabResult read = prefabEntityFromJson(json, entity); !read) {
            return read;
        }
        if (member == root) {
            entity.parent.reset();
            if (rootTransform) {
                entity.localTransform = *rootTransform;
            }
        }
        captured.entityIds.emplace_back(scene.id(member), entity.id);
        captured.document.entities.push_back(std::move(entity));
    }

    const std::filesystem::path directory = path.parent_path();
    for (const auto& [nested, id] : nestedIds) {
        const auto& nestedInstance = scene.registry().get<PrefabInstanceComponent>(nested);
        const auto* nestedRootMarker = scene.registry().try_get<PrefabEntityComponent>(nested);
        if (nestedRootMarker == nullptr) {
            return {.error = "Nested prefab instance '" + scene.registry().get<NameComponent>(nested).name +
                             "' is missing its prefab link"};
        }
        std::filesystem::path nestedPath;
        if (PrefabResult resolved = resolve(nestedInstance.prefab, nestedPath); !resolved) {
            return resolved;
        }
        NestedPrefab record{.id = id,
                            .source = nestedPath.lexically_relative(directory),
                            .parent = PrefabEntityId{sceneToLocal.at(scene.id(parentOf(scene, nested)).value)}};
        const std::string& nestedName = scene.registry().get<NameComponent>(nested).name;
        std::vector<PrefabOverride> live;
        if (PrefabResult result = overrides(scene, nested, live); !result) {
            return {.error = "Nested prefab instance '" + nestedName + "': " + result.error};
        }
        record.overrides = capturedRecordOverrides(std::move(live), sceneToLocal, types, nestedName, diagnostics);
        // The nested root's placement inside the containing prefab.
        record.overrides.push_back(
            {.entity = nestedRootMarker->entity,
             .field = "/local_transform",
             .jsonValue = matrixJson(scene.registry().get<TransformComponent>(nested).local).dump()});
        captured.nestedIds.emplace_back(scene.id(nested), id);
        captured.document.nested.push_back(std::move(record));
    }
    if (PrefabResult checked = checkNestedGraph(reference, captured.document); !checked) {
        return checked;
    }
    output = std::move(captured);
    return {.success = true, .diagnostics = std::move(diagnostics)};
}
