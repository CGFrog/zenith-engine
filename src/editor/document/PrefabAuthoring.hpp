#pragma once

#include "engine/scene/Prefab.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

// Structural changes are overrides too: deleting a prefab entity from an
// instance (through the editor's delete commands, which record it with
// recordPrefabEntityRemoval) and moving one under another entity of the
// instance survive refreshes; revert restores them and apply writes them into
// the prefab.
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

  private:
    PrefabSystem m_prefabs;
};

// Keeps one entity's overrides between frames, so a panel does not reload and
// compare the prefab every frame. A result is reused while the entity, the
// document revision (CommandHistory::revision), and the prefab file's write
// time stay the same.
class PrefabOverrideCache {
  public:
    // The entity's overrides; on failure they are empty (not linked) and
    // error() says why.
    const PrefabEntityOverrides& get(const PrefabAuthoring& authoring, const Scene& scene, SceneEntityId entity,
                                     uint64_t revision);

    const std::string& error() const {
        return m_error;
    }

    void clear();

  private:
    struct Key {
        SceneEntityId entity;
        uint64_t revision{0};
        // Scene::contentGeneration, so a replaced scene (Play mode) is reread.
        uint64_t sceneGeneration{0};
        std::string prefab;
        std::filesystem::file_time_type prefabWriteTime{};

        bool operator==(const Key&) const = default;
    };

    std::optional<Key> m_key;
    PrefabEntityOverrides m_overrides;
    std::string m_error;
};
