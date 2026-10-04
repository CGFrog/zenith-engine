#pragma once

#include "engine/scene/Prefab.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class AssetManager;

// One component's overridden values on an entity of a prefab instance.
struct PrefabComponentOverrides {
    // Component type key, for example "point_light".
    std::string component;
    // RFC 6901 pointers into the component's document object, sorted. The
    // empty pointer means the whole component (added, replaced, or removed).
    std::vector<std::string> fields;
    // The instance added a component the prefab entity lacks.
    bool added{false};
    // The instance removed a component the prefab entity has.
    bool removed{false};
};

// How an entity of a prefab instance differs from its prefab, for override
// highlighting and per-field revert.
struct PrefabEntityOverrides {
    // False when the entity is not part of a prefab instance; everything
    // below is then empty.
    bool linked{false};
    SceneEntityId instanceRoot;
    // Project-relative path of the instance's prefab.
    std::string prefab;
    // The prefab's display name (PrefabDocument::displayName, or its file
    // name when it has none).
    std::string displayName;
    // The entity was added to the instance in the scene rather than created
    // from the prefab, so it has no prefab values to compare with.
    bool added{false};
    bool name{false};
    bool localTransform{false};
    // Moved under another entity of the instance.
    bool parent{false};
    // Sorted by component key.
    std::vector<PrefabComponentOverrides> components;
    // The whole instance's override count, and how many of those are prefab
    // entities deleted from it, for the instance header.
    size_t instanceOverrides{0};
    size_t removedEntities{0};

    // Whether a component field (a JSON pointer into the component's document
    // object), a value containing it, or a value inside it is overridden. The
    // empty pointer asks about the component as a whole. With an empty
    // component, field is "/name", "/local_transform", or "/parent".
    bool overridden(std::string_view component, std::string_view field) const;
};

// Structural changes are overrides too: deleting a prefab entity or a nested
// instance from an instance (through the editor's delete commands, which
// record it with recordPrefabEntityRemoval), moving one under another entity
// of the instance (including entities added in the scene), and moving one out
// of the instance (outliner.parent_drop records it with
// recordPrefabEntityMove: it is removed from the instance and stays in the
// scene unlinked) survive refreshes; revert restores them and apply writes
// them into the prefab.
//
// Prefab authoring workflows on an open scene. Each call edits the scene
// directly and records no history: callers (the prefab operators) wrap them in
// one undo step. Prefab files are written immediately and are not part of
// undo; see PrefabOperators.hpp. References are project-relative paths under
// options.assetRoot. Holds references; assets and options must outlive it.
class PrefabAuthoring {
  public:
    PrefabAuthoring(AssetManager& assets, const SceneDocumentOptions& options);

    // Writes root's subtree as a new prefab file at reference (which must end
    // in .prefab and not exist yet) and links root to it as an instance.
    // Instances inside the subtree become nested prefabs. An entity inside an
    // instance (other than its root) must be unpacked first.
    PrefabResult create(Scene& scene, Entity root, const std::filesystem::path& reference);
    // Instantiates a prefab under parent (the scene root when null).
    PrefabResult instantiate(Scene& scene, const std::filesystem::path& reference, Entity parent, Entity& root);
    // Writes the instance containing entity back into its prefab file, then
    // refreshes every other instance of that prefab in the scene with the new
    // content while keeping their own overrides.
    PrefabResult apply(Scene& scene, Entity entity);
    // Resets the instance containing entity to its prefab: overrides are
    // dropped and entities added in the scene are removed.
    PrefabResult revert(Scene& scene, Entity entity);
    // Resets one overridden value of entity: field is a JSON pointer into the
    // component's document object (empty for the whole component), or, with
    // an empty component, "/name" or "/local_transform".
    PrefabResult revertField(Scene& scene, Entity entity, std::string_view component, std::string_view field);
    // Keeps the instance's entities but removes their link to the prefab.
    PrefabResult unpack(Scene& scene, Entity entity);

    // The overrides of one entity of an instance; reads the prefab file.
    PrefabResult overrides(const Scene& scene, Entity entity, PrefabEntityOverrides& output) const;

    // A free project-relative path for a new prefab made from entity:
    // "prefabs/<name>.prefab", numbered when that file exists.
    std::filesystem::path suggestPath(const Scene& scene, Entity entity) const;
    // The file a project-relative prefab reference names.
    PrefabResult resolve(const std::filesystem::path& reference, std::filesystem::path& path) const;

    const PrefabSystem& prefabs() const {
        return m_prefabs;
    }

  private:
    PrefabSystem m_prefabs;
};

// Display names of prefab files for lists such as Add > Prefab; each file is
// read once per write time.
class PrefabDisplayNames {
  public:
    // The prefab's display name, or its file name without the extension when
    // it has none or cannot be read.
    const std::string& get(const std::filesystem::path& file);

  private:
    struct Entry {
        std::filesystem::file_time_type writeTime{};
        std::string name;
    };

    std::unordered_map<std::string, Entry> m_entries;
};

// Keeps an instance's overrides between frames, so a panel does not reload
// and compare the prefab every frame. The prefab is loaded and prepared once
// per file write time; the whole instance is compared once per instance,
// prefab file, and scene (Scene::contentGeneration). After an edit (a new
// document revision, CommandHistory::revision) only the shown entity is
// compared again, which is exact for its own highlights, so a drag stays
// cheap; the instance-wide counts (other entities edited in the same step,
// removals) are refreshed by a full comparison once revisions have stopped
// changing for settleDelay. Showing another entity of the same instance reuses
// the instance's overrides.
class PrefabOverrideCache {
  public:
    explicit PrefabOverrideCache(std::chrono::milliseconds settleDelay = std::chrono::milliseconds(250));

    // The entity's overrides; on failure they hold only the link (when the
    // entity is in an instance) and error() says why.
    const PrefabEntityOverrides& get(const PrefabAuthoring& authoring, const Scene& scene, SceneEntityId entity,
                                     uint64_t revision);

    const std::string& error() const {
        return m_error;
    }

    // Whether the instance's overrides were last updated for one entity only,
    // so instance-wide counts may lag until the edits settle.
    bool partial() const {
        return m_partial;
    }

    void clear();

  private:
    struct SourceKey {
        std::string prefab;
        std::filesystem::file_time_type writeTime{};

        bool operator==(const SourceKey&) const = default;
    };

    struct InstanceKey {
        SceneEntityId root;
        uint64_t revision{0};
        // Scene::contentGeneration, so a replaced scene (Play mode) is reread.
        uint64_t sceneGeneration{0};
        std::string prefab;
        std::filesystem::file_time_type prefabWriteTime{};

        bool operator==(const InstanceKey&) const = default;
    };

    // Brings the prepared prefab and the instance's overrides up to key;
    // returns whether they (or the error) changed.
    bool update(const PrefabAuthoring& authoring, const Scene& scene, Entity root, Entity entity, bool added,
                const InstanceKey& key);

    std::chrono::milliseconds m_settleDelay;
    std::optional<SourceKey> m_sourceKey;
    PrefabComparisonSource m_source;
    std::string m_sourceError;
    std::optional<InstanceKey> m_instanceKey;
    std::vector<PrefabOverride> m_instanceOverrides;
    bool m_partial{false};
    std::chrono::steady_clock::time_point m_lastPartialUpdate{};
    // The entity and instance state m_overrides describes.
    std::optional<std::pair<SceneEntityId, InstanceKey>> m_result;
    PrefabEntityOverrides m_overrides;
    std::string m_error;
};
