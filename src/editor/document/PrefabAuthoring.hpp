#pragma once

#include "engine/scene/Prefab.hpp"

#include <filesystem>
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
    // Sorted by component key.
    std::vector<PrefabComponentOverrides> components;

    // Whether a component field (a JSON pointer into the component's document
    // object), a value containing it, or a value inside it is overridden. The
    // empty pointer asks about the component as a whole.
    bool overridden(std::string_view component, std::string_view field) const;
};

// Overrides cover values only: deleting a prefab entity from an instance or
// moving one under another parent is not an override, so the next refresh of
// that instance (revert, revertField, or another instance's apply) restores it.
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

  private:
    PrefabSystem m_prefabs;
};
