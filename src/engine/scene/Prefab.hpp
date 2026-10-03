#pragma once

#include "Scene.hpp"
#include "SceneDocument.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class AssetManager;
class ComponentTypeRegistry;

// Prefabs: reusable entity hierarchies stored as files under the project asset
// root and instantiated into scenes with a persisted link.
//
// A prefab document lists its own entities in the scene document's entity form
// (name, local transform, parent, and component objects keyed by registered
// component type), with ids local to the prefab, plus nested prefab records.
// Component objects are read and written by the ComponentTypeRegistry, so any
// registered component (including game module components) can be part of a
// prefab, and unknown component keys survive a load as preserved components.
//
// An instance is the instance root (which holds PrefabInstanceComponent) and
// every descendant up to, but not including, the root of another instance. An
// instance entity created from the prefab holds PrefabEntityComponent naming
// its prefab-local id; descendants without it were added to the instance in the
// scene. A nested prefab record instantiates as a separate instance whose root
// remembers the record (PrefabInstanceComponent::nestedId).

struct PrefabEntityId {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    auto operator<=>(const PrefabEntityId&) const = default;
};

struct PrefabEntity {
    PrefabEntityId id;
    std::string name;
    // Null for the prefab root; every document has exactly one root.
    std::optional<PrefabEntityId> parent;
    glm::mat4 localTransform{1.0f};
    // Component document objects keyed by component type key, as compact JSON
    // in the same form SceneDocument writes them. Sorted by key.
    std::vector<std::pair<std::string, std::string>> components;
};

// One overridden value of one prefab entity. Fields are RFC 6901 JSON pointers:
//
//   component ""            field "/name" or "/local_transform"
//   component "point_light" field "/intensity" (a value inside the component)
//   component "point_light" field ""  value {...}  (component added or replaced)
//   component "point_light" field ""  value null   (component removed)
struct PrefabOverride {
    PrefabEntityId entity;
    std::string component;
    std::string field;
    // Compact JSON.
    std::string jsonValue;

    bool operator==(const PrefabOverride&) const = default;
};

struct NestedPrefab {
    // Unique within the containing document; instances remember it.
    uint64_t id{0};
    // Relative to the directory of the containing prefab.
    std::filesystem::path source;
    // The containing prefab's entity the nested root is parented to; null
    // parents it to the containing prefab's root.
    std::optional<PrefabEntityId> parent;
    std::vector<PrefabOverride> overrides;
};

// Prefab document format history. Older documents are upgraded on load and
// saved as the current version.
//
//   1  `localTransform`, an optional `modelAsset` path, and only `pointLight`
//      and `camera` components; overrides named `name`, `localTransform`,
//      `component.pointLight`, and `component.camera`; any number of roots.
//   2  Entities in the scene document entity form with registry component keys
//      and asset reference objects; field-level JSON pointer overrides;
//      exactly one root.
struct PrefabDocument {
    static constexpr uint32_t currentVersion = 2;
    std::vector<PrefabEntity> entities;
    std::vector<NestedPrefab> nested;
};

struct PrefabResult {
    bool success{false};
    std::string error;
    // Recoverable problems, for example overrides whose target no longer
    // exists in the prefab or version 1 data that could not be migrated.
    std::vector<std::string> diagnostics;

    explicit operator bool() const {
        return success;
    }
};

// The document form of an override, shared by prefab documents and the
// `prefab_instance` component: {"entity", "component", "field", "value"}.
nlohmann::json prefabOverrideToJson(const PrefabOverride& value);
PrefabResult prefabOverrideFromJson(const nlohmann::json& json, PrefabOverride& output);

// The scene document entity form of a prefab entity ({id, name, parent,
// local_transform, components}), with prefab-local ids.
PrefabResult prefabEntityToJson(const PrefabEntity& entity, nlohmann::json& output);
PrefabResult prefabEntityFromJson(const nlohmann::json& json, PrefabEntity& output);

PrefabResult serializePrefab(const PrefabDocument& prefab, std::string& output);
PrefabResult deserializePrefab(std::string_view input, PrefabDocument& prefab);
// Writes atomically, creating missing parent directories.
PrefabResult savePrefab(const std::filesystem::path& path, const PrefabDocument& prefab);
PrefabResult loadPrefab(const std::filesystem::path& path, PrefabDocument& prefab);

// Links an instance root to its prefab. Persisted as `prefab_instance`.
struct PrefabInstanceComponent {
    // Project-relative path of the prefab asset; saved as an asset reference
    // ({guid, path}) so the link follows renamed or moved prefabs.
    std::string prefab;
    // The instance's overrides as of its last prefab operation (instantiate,
    // apply, revert, refresh). Live overrides are the instance's differences
    // from its prefab; see PrefabSystem::overrides.
    std::vector<PrefabOverride> overrides;
    // The nested prefab record of the enclosing prefab this instance was
    // created from, or zero for an instance placed directly in the scene.
    uint64_t nestedId{0};
};

// Names the prefab-local id of an instance entity. Persisted as `prefab_entity`.
struct PrefabEntityComponent {
    PrefabEntityId entity;
};

// Registers `prefab_instance` and `prefab_entity`; both are internal.
SceneDocumentResult registerPrefabComponentTypes(ComponentTypeRegistry& registry);

// The nearest ancestor-or-self of entity holding PrefabInstanceComponent, or
// null when entity is not part of an instance.
Entity prefabInstanceRoot(const Scene& scene, Entity entity);
// The instance's entities: the root first, then its descendants depth first,
// excluding nested instances (whose roots are listed in nestedRoots when given).
std::vector<Entity> prefabInstanceMembers(const Scene& scene, Entity root, std::vector<Entity>* nestedRoots = nullptr);
// Every instance root in the scene whose prefab is reference.
std::vector<Entity> prefabInstancesOf(const Scene& scene, const std::filesystem::path& reference);
// Removes the instance's link: the root's PrefabInstanceComponent and every
// member's PrefabEntityComponent. Nested instances stay linked to their own
// prefabs as instances placed directly in the scene.
void unpackPrefabInstance(Scene& scene, Entity root);

struct PrefabInstanceResult {
    bool success{false};
    Entity root{entt::null};
    // Every entity of the instance, including nested instances.
    std::vector<Entity> entities;
    std::vector<std::string> diagnostics;
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

enum class PrefabRefreshMode : uint8_t {
    // Entities and nested instances added to the instance in the scene stay.
    KeepAddedObjects,
    // The instance is reset to exactly what the prefab describes: added
    // entities are removed and nested instances are refreshed from their
    // records.
    RemoveAddedObjects,
};

// A scene subtree written as a prefab document, with the prefab-local id given
// to each captured scene entity and the record id given to each nested
// instance root.
struct PrefabCapture {
    PrefabDocument document;
    std::vector<std::pair<SceneEntityId, PrefabEntityId>> entityIds;
    std::vector<std::pair<SceneEntityId, uint64_t>> nestedIds;
};

// Instantiates prefabs into scenes and compares instances with their prefabs.
// Runtime code: it needs no editor state. Prefab references are paths
// relative to options.assetRoot; options.componentTypes decides which
// components prefabs read and write. The asset manager must outlive the
// system.
class PrefabSystem {
  public:
    PrefabSystem(AssetManager& assets, SceneDocumentOptions options);

    // Creates an instance under parent (the scene root when null) with the
    // given overrides, which become its persisted overrides. Nested prefabs
    // become nested instances. Missing sources, nested cycles, and invalid
    // components fail before the scene changes.
    PrefabInstanceResult instantiate(const std::filesystem::path& reference, Scene& scene, Entity parent = entt::null,
                                     std::span<const PrefabOverride> overrides = {});
    // Rebuilds an instance from its (possibly changed) prefab plus overrides,
    // which become its persisted overrides. The root keeps its parent and,
    // unless an override sets it, its local transform; entities still in the
    // prefab keep their stable ids; entities the prefab no longer has are
    // removed; missing nested instances are created.
    PrefabInstanceResult refresh(Scene& scene, Entity root, std::span<const PrefabOverride> overrides,
                                 PrefabRefreshMode mode);
    // Refreshes with the instance's persisted overrides.
    PrefabInstanceResult refresh(Scene& scene, Entity root, PrefabRefreshMode mode);

    // The instance's current differences from its prefab file, sorted by
    // entity, component, and field. The root's local transform is the
    // instance's placement and never reported. Entities added in the scene
    // are not reported.
    PrefabResult overrides(const Scene& scene, Entity root, std::vector<PrefabOverride>& output) const;
    // The same, against a given document of the instance's prefab (for
    // example its content before an apply replaced the file).
    PrefabResult overrides(const Scene& scene, Entity root, const PrefabDocument& source,
                           std::vector<PrefabOverride>& output) const;

    // Writes root's subtree as a document to be saved at reference. Nested
    // instances become nested records carrying their overrides. When root is
    // an instance of reference itself (an apply), its entities keep their
    // prefab-local ids and nested records keep their ids; otherwise ids are
    // assigned depth first from 1. Fails when saving would make a nested
    // cycle.
    PrefabResult capture(const Scene& scene, Entity root, const std::filesystem::path& reference,
                         PrefabCapture& output) const;

    // Resolves reference under the asset root and loads its document.
    PrefabResult load(const std::filesystem::path& reference, PrefabDocument& document) const;
    // The file a project-relative reference names; it need not exist yet.
    PrefabResult resolve(const std::filesystem::path& reference, std::filesystem::path& path) const;

    const SceneDocumentOptions& options() const {
        return m_options;
    }

  private:
    // Runs change and, when it fails, puts the scene back as it was.
    PrefabInstanceResult transact(Scene& scene, const std::function<PrefabInstanceResult()>& change);
    PrefabInstanceResult build(const std::filesystem::path& reference, Scene& scene, Entity existingRoot, Entity parent,
                               std::span<const PrefabOverride> overrides, uint64_t nestedId, PrefabRefreshMode mode,
                               std::vector<std::filesystem::path>& stack);
    PrefabResult checkNestedGraph(const std::filesystem::path& reference, const PrefabDocument& document) const;

    AssetManager& m_assets;
    SceneDocumentOptions m_options;
};
