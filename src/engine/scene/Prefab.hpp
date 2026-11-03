#pragma once

// Prefab documents: entities, overrides, nested prefabs; PrefabSystem API.

#include "Scene.hpp"
#include "SceneDocument.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
//   component ""            field "/parent"  value <prefab-local id>
//                           (moved under another entity of the instance), or
//                           <scene id | 1 << 63> (moved under an entity added
//                           to the instance in the scene)
//   component ""            field "/removed" value true
//                           (deleted from the instance, with its descendants)
//   component "point_light" field "/intensity" (a value inside the component)
//   component "point_light" field ""  value {...}  (component added or replaced)
//   component "point_light" field ""  value null   (component removed)
//
// An override with a non-zero `nested` targets the nested prefab record of
// that id instead of an entity (entity is then zero). Its only form is field
// "/removed" value true: the nested instance was deleted from the containing
// instance. Persisted as {"nested", "component", "field", "value"}.
//
// Entity references inside override values hold the values a prefab document
// holds for entities of the instance (see PrefabDocument), and for entities
// outside it their scene id with the top bit (1 << 63) set, so the two cannot
// be confused. In a prefab file, the overrides of a nested record use the top
// bit for the containing prefab's entities instead (their prefab-local id), so
// prefab files never hold scene ids. Scene-local components (collection
// membership and collection instances) are never overrides: they are scene
// organization, which a prefab does not store and a refresh keeps.
struct PrefabOverride {
    PrefabEntityId entity;
    std::string component;
    std::string field;
    // Compact JSON.
    std::string jsonValue;
    // The nested record the override targets, or zero for an entity override.
    uint64_t nested{0};

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
//      exactly one root. An optional `metadata` object holds `display_name`.
//
// Entity references inside a document's components hold a prefab-local id, or,
// for an entity of a nested instance, prefabNestedReference(record, local id
// in the nested prefab) (bit 62 set). References into instances nested more
// than one level deep are not stored.
struct PrefabDocument {
    static constexpr uint32_t currentVersion = 2;
    std::vector<PrefabEntity> entities;
    std::vector<NestedPrefab> nested;
    // The name people see (the Add menu, the instance header). Prefab file
    // names are reduced to portable ASCII, so this keeps the name the prefab
    // was created from; empty means the file name.
    std::string displayName;
};

// The reference value naming entity local of the nested record's instance, or
// zero when the ids do not fit (records above 2^30, entities above 2^32).
uint64_t prefabNestedReference(uint64_t record, PrefabEntityId local);

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
// Sorts overrides into the order PrefabSystem::overrides reports them in:
// by nested record, then entity, component, and field.
void sortPrefabOverrides(std::vector<PrefabOverride>& overrides);

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
// Records, on its instance root's persisted overrides, that entity (a prefab
// entity of an instance, not the root) is about to be deleted, so refreshes
// keep it deleted until the instance is reverted or the prefab applied. Call
// it before destroying the entity; descendants need no record of their own.
// A nested instance root created from a record of the containing instance is
// recorded on the containing instance as a removed nested record. Returns
// false, changing nothing, for entities outside instances, instance roots
// placed in the scene, and entities added to an instance in the scene.
bool recordPrefabEntityRemoval(Scene& scene, Entity entity);
// Prepares entity for being parented to newParent (the scene root when null).
// When that moves it out of the instance it belongs to (to the scene root,
// under an entity of no instance, or into another instance, nested ones
// included), the move is recorded as the removal of its prefab entities and
// nested instances (as recordPrefabEntityRemoval does), and the moved subtree
// is unlinked: its entities become plain scene entities that keep their
// components, and nested instances in it become instances placed in the
// scene. Moves within the instance, including under entities added to it in
// the scene, need no record: they are live /parent overrides. Call it before
// reparenting; returns whether it changed anything.
bool recordPrefabEntityMove(Scene& scene, Entity entity, Entity newParent);

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

// How PrefabSystem::capture treats the captured root.
struct PrefabCaptureOptions {
    // When root is an instance of the reference being captured (an apply),
    // write root's own local transform as the prefab root's instead of
    // keeping the previous file's. An apply leaves it false, since an
    // instance root's transform only places that instance; a prefab opened
    // as a document of its own (instantiated into an empty scene) sets it,
    // since there the root's transform is the prefab's content.
    bool keepRootTransform{false};
};

// A prefab document prepared for comparing instances with it (see
// PrefabSystem::prepareComparison), so repeated comparisons, such as override
// highlighting while a value is dragged, skip normalizing the prefab.
// Reusable while the document is unchanged.
struct PrefabComparisonSource {
    PrefabDocument document;
    // Each entity in the canonical serialized form instance entities are
    // compared in (compact JSON), keyed by prefab-local id.
    std::map<uint64_t, std::string> entities;
};

// How the prefab document cache has been used since the system was created.
struct PrefabCacheStats {
    // Lookups served from a cached document whose file was unchanged.
    uint64_t hits{0};
    // Lookups that read and parsed the file: first use, a changed file, or an
    // invalidated entry.
    uint64_t loads{0};
    // Documents cached now.
    size_t entries{0};
};

// Instantiates prefabs into scenes and compares instances with their prefabs.
// Runtime code: it needs no editor state. Prefab references are paths
// relative to options.assetRoot; options.componentTypes decides which
// components prefabs read and write. The asset manager must outlive the
// system.
//
// Instantiating and refreshing read prefab files through a document cache
// keyed by resolved path: a document is parsed and checked once per file
// version (its write time and size) and reused by every instance after that.
// A stat costs more than building a small prefab, so a file is compared with
// its cached version at most once per file check interval; code that writes a
// prefab and spawns it at once (the editor's apply, its asset watcher) calls
// invalidate(). load() always reads the file. Not thread-safe: use one system
// per thread.
class PrefabSystem {
  public:
    PrefabSystem(AssetManager& assets, SceneDocumentOptions options);

    // Creates an instance under parent (the scene root when null) with the
    // given overrides, which become its persisted overrides. Nested prefabs
    // become nested instances. Missing sources, nested cycles, and invalid
    // components fail without changing the scene's entities.
    //
    // Entities are built straight from the cached documents through the
    // component codecs. The cached graph (every reachable document loads,
    // its components read, and nesting has no cycle) is checked before the
    // scene changes; anything that still fails afterwards (an override
    // holding an invalid value, say) destroys what the call created. A failed
    // call may leave Scene::nextEntityId advanced.
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
    // are not reported; prefab entities the instance lacks are reported as
    // removed only when their deletion was recorded (recordPrefabEntityRemoval),
    // since a prefab entity added after the last refresh is missing too.
    PrefabResult overrides(const Scene& scene, Entity root, std::vector<PrefabOverride>& output) const;
    // The same, against a given document of the instance's prefab (for
    // example its content before an apply replaced the file).
    PrefabResult overrides(const Scene& scene, Entity root, const PrefabDocument& source,
                           std::vector<PrefabOverride>& output) const;
    // The same, against a prepared document.
    PrefabResult overrides(const Scene& scene, Entity root, const PrefabComparisonSource& source,
                           std::vector<PrefabOverride>& output) const;
    // Only the value and parent overrides of entity, a member of the instance
    // at root, sorted as overrides() sorts them; empty for an entity added in
    // the scene. Much cheaper than comparing the whole instance.
    PrefabResult entityOverrides(const Scene& scene, Entity root, Entity entity, const PrefabComparisonSource& source,
                                 std::vector<PrefabOverride>& output) const;
    PrefabResult prepareComparison(const PrefabDocument& document, PrefabComparisonSource& output) const;

    // Writes root's subtree as a document to be saved at reference. Nested
    // instances become nested records carrying their overrides. When root is
    // an instance of reference itself (an apply), its entities keep their
    // prefab-local ids, nested records keep their ids, and the document keeps
    // its display name; otherwise ids are assigned depth first from 1 and the
    // display name is root's name. Fails when saving would make a nested
    // cycle. Entity references between captured entities become prefab-local
    // ids, and references to entities of nested instances become nested
    // references (prefabNestedReference); references to anything else become
    // none and are reported as diagnostics. Nested records' overrides name
    // captured entities with the top bit set and drop other scene references
    // the same way. Scene-local components are not captured. An apply keeps
    // the previous file's root transform unless options.keepRootTransform.
    PrefabResult capture(const Scene& scene, Entity root, const std::filesystem::path& reference,
                         PrefabCapture& output) const;
    // A separate overload rather than a defaulted parameter, so game modules
    // built against the four-argument form keep loading.
    PrefabResult capture(const Scene& scene, Entity root, const std::filesystem::path& reference, PrefabCapture& output,
                         const PrefabCaptureOptions& options) const;

    // Resolves reference under the asset root and loads its document.
    PrefabResult load(const std::filesystem::path& reference, PrefabDocument& document) const;
    // The file a project-relative reference names; it need not exist yet.
    PrefabResult resolve(const std::filesystem::path& reference, std::filesystem::path& path) const;

    const SceneDocumentOptions& options() const {
        return m_options;
    }

    // Forgets the cached document of one prefab, named by a project-relative
    // reference or an absolute file path, so its next use reads the file.
    void invalidate(const std::filesystem::path& prefab);
    // Forgets every cached document.
    void clearCache();
    // How long a cached document is trusted before its file is compared
    // again; zero compares on every use.
    static constexpr std::chrono::milliseconds defaultFileCheckInterval{500};
    void setFileCheckInterval(std::chrono::milliseconds interval);
    PrefabCacheStats cacheStats() const;

  private:
    // Immutable once cached, so copies of the system share them.
    struct CachedPrefab;

    // The cached document of the file at path (resolved), loading it when it
    // is not cached or its file changed. Never null.
    std::shared_ptr<const CachedPrefab> cached(const std::filesystem::path& path);
    // Whether every prefab instantiating reference with overrides reaches
    // loads and reads, with no nested cycle; nested records the overrides
    // remove are skipped.
    bool cachedGraphValid(const std::filesystem::path& reference, std::span<const PrefabOverride> overrides,
                          std::vector<std::filesystem::path>& stack);
    // Runs change and, when it fails, puts the scene back as it was. change
    // may rebuild root's subtree (none for a new instance) and create
    // entities, and must change nothing else.
    PrefabInstanceResult transact(Scene& scene, Entity root, const std::function<PrefabInstanceResult()>& change);
    PrefabInstanceResult build(const std::filesystem::path& reference, Scene& scene, Entity existingRoot, Entity parent,
                               std::span<const PrefabOverride> overrides, uint64_t nestedId, PrefabRefreshMode mode,
                               std::vector<std::filesystem::path>& stack);
    PrefabResult checkNestedGraph(const std::filesystem::path& reference, const PrefabDocument& document) const;

    AssetManager& m_assets;
    SceneDocumentOptions m_options;
    // Keyed by the resolved path's generic string.
    std::unordered_map<std::string, std::shared_ptr<const CachedPrefab>> m_cache;
    PrefabCacheStats m_cacheStats;
    std::chrono::milliseconds m_fileCheckInterval{defaultFileCheckInterval};
};
