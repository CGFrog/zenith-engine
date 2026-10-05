#pragma once

#include "Scene.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class AssetDatabase;
class AssetManager;
class ComponentTypeRegistry;

// What a document is for: a scene (the default) leaves fields marked
// .runtimeState() out; a save game keeps them.
enum class SceneDocumentPurpose {
    Scene,
    SaveGame,
};

struct SceneDocumentOptions {
    std::filesystem::path assetRoot;
    // Component types the document reads and writes. Null uses the built-in
    // engine types; games that add their own pass a registry extending them.
    std::shared_ptr<const ComponentTypeRegistry> componentTypes;
    // Identifies assets by GUID. Saving records each referenced asset's GUID
    // beside its path; loading resolves a reference by GUID first, so a scene
    // keeps working after its assets are renamed or moved. The database may
    // cover the asset root or any directory containing it. Null writes
    // path-only references, which load by path. The caller keeps the database
    // current (rescanned) and must not mutate it during a document call.
    std::shared_ptr<const AssetDatabase> assetDatabase;
    // Read by writers; readers accept runtime-state fields either way.
    SceneDocumentPurpose purpose{SceneDocumentPurpose::Scene};
};

struct SceneDocumentResult {
    bool success{false};
    std::string error;
    // Problems that did not stop the operation, for example a saved
    // reference to a transient entity written as none.
    std::vector<std::string> warnings;

    explicit operator bool() const {
        return success;
    }
};

class ParsedSceneDocument {
  public:
    ParsedSceneDocument() = default;

    explicit operator bool() const {
        return static_cast<bool>(m_impl);
    }

  private:
    friend class SceneDocument;
    struct Impl;
    std::shared_ptr<const Impl> m_impl;
};

// One entity's document form on each side of an edit: its compact JSON entity
// object exactly as SceneDocument::serialize writes it (id, name,
// local_transform, parent, components), or nullopt when the entity does not
// exist on that side.
struct SceneEntityChange {
    SceneEntityId id;
    std::optional<std::string> before;
    std::optional<std::string> after;
};

// The scene's collection definitions on each side of an edit: the compact
// JSON `collections` array exactly as SceneDocument::serialize writes it.
struct SceneCollectionsChange {
    std::string before;
    std::string after;
};

// Everything an edit changed between two documents: the entities, sorted by
// id, and the collection definitions when they differ.
struct SceneChanges {
    std::vector<SceneEntityChange> entities;
    std::optional<SceneCollectionsChange> collections;

    bool empty() const {
        return entities.empty() && !collections;
    }
};

// One entity SceneDocument::createEntities adds: its stable id and its parsed
// document entity object (name, local_transform, parent, components; any id
// field is ignored). The object must outlive the call.
struct SceneEntityDocument {
    SceneEntityId id;
    const nlohmann::json* entity{nullptr};
};

enum class SceneChangeDirection {
    // Moves each changed entity to its after state.
    Forward,
    // Moves each changed entity back to its before state.
    Backward,
};

// Scene document format history. Each version above 1 has a document-level
// migration that upgrades the raw JSON of the previous version before it is
// validated, so older files load unchanged in behavior and save as the current
// version. Newer versions are rejected.
//
//   1  Asset references are project-relative path strings, or `{"path": ...}`
//      inside model, mesh, and skin components.
//   2  Every asset reference is an object holding its project-relative `path`
//      and, when an asset database knows the asset, its `guid`. An empty
//      string still means "no asset".
//   3  A top-level `collections` array, sorted by id, holds the collection
//      definitions: {id, name, parent (id or null), visible, selectable,
//      renderable, instance_offset [x, y, z]}. Entities join collections
//      through the `collection_membership` component and instance them
//      through `collection_instance`; both must name collections the
//      document defines. Version 2 documents migrate with no collections.
//
// Transient entities (TransientComponent, Scene::isTransient) are never
// written: serialize and save leave them out, serializeEntities reports them
// as absent, and a written entity's nullable reference to one (a reflected
// entity reference field) is written as none, with a warning from serialize
// and save, so a saved document never names an entity it does not contain.
// applyChanges leaves transient entities alone and lets a removal take its
// transient children with it.
class SceneDocument {
  public:
    static constexpr uint32_t currentVersion = 3;

    static SceneDocumentResult serialize(const Scene& scene, const AssetManager& assets,
                                         const SceneDocumentOptions& options, std::string& output);
    static SceneDocumentResult deserialize(std::string_view input, AssetManager& assets,
                                           const SceneDocumentOptions& options, Scene& destination);
    static SceneDocumentResult parse(std::string_view input, ParsedSceneDocument& output);
    static SceneDocumentResult commit(const ParsedSceneDocument& document, AssetManager& assets,
                                      const SceneDocumentOptions& options, Scene& destination);
    static SceneDocumentResult save(const std::filesystem::path& path, const Scene& scene, const AssetManager& assets,
                                    const SceneDocumentOptions& options);
    static SceneDocumentResult load(const std::filesystem::path& path, AssetManager& assets,
                                    const SceneDocumentOptions& options, Scene& destination);

    // Each listed entity's document form exactly as a SceneEntityChange holds
    // it (the compact JSON diff writes), or nullopt for an id no entity has,
    // without serializing the rest of the scene. Collections are not written.
    static SceneDocumentResult serializeEntities(const Scene& scene, const AssetManager& assets,
                                                 const SceneDocumentOptions& options,
                                                 std::span<const SceneEntityId> ids,
                                                 std::vector<std::optional<std::string>>& output);
    // The scene's collection definitions exactly as a SceneCollectionsChange
    // holds them (the compact JSON collections array diff writes), without
    // serializing any entity.
    static SceneDocumentResult serializeCollections(const Scene& scene, std::string& output);

    // Lists, sorted by id, the entities whose document form differs between
    // two serialized documents, and the collection definitions when they
    // differ. Entities equal on both sides are omitted, so the result is
    // empty when the documents describe the same scene.
    static SceneDocumentResult diff(std::string_view before, std::string_view after, SceneChanges& changes);

    // Patches destination in place so every listed entity matches its state
    // on the chosen side: absent entities are destroyed, others are created
    // or rebuilt from their JSON with the same stable id. Unlisted entities
    // are untouched and keep their EnTT handles, components, and child order.
    // Listed collection definitions replace the scene's.
    //
    // A rebuilt entity keeps its EnTT handle, loses every component the
    // document does not describe (as a full document load would), and keeps
    // its position among its siblings when its parent is unchanged. An entity
    // that is created or moves to another parent is inserted before the first
    // sibling with a larger stable id, which is the order a full document load
    // produces when siblings are already in id order.
    //
    // The patch is atomic: entity JSON, components, parent references,
    // hierarchy cycles, children orphaned by a removal, collection
    // definitions, and every collection reference of the resulting scene are
    // all validated before the scene changes, and a failure leaves
    // destination untouched. Removing an entity that does not exist is not an
    // error, so a patch can be applied to a scene already in the target state.
    static SceneDocumentResult applyChanges(const SceneChanges& changes, SceneChangeDirection direction,
                                            AssetManager& assets, const SceneDocumentOptions& options,
                                            Scene& destination);

    // Adds new entities from parsed document entity objects, reading their
    // components through the component codecs straight into destination, so
    // it skips the text round trip and staging scene applyChanges uses. The
    // result matches an applyChanges that creates the same entities: each is
    // attached to its parent (another new entity or an existing one, or the
    // root when null) before the first sibling with a larger stable id.
    //
    // Ids, parents, and hierarchy cycles are checked before the scene
    // changes; a component that fails to read, or a collection reference the
    // scene does not define, destroys every entity the call created before it
    // returns the error. The scene's entities are then as before, though ids
    // the call used are not handed out again (Scene::nextEntityId).
    static SceneDocumentResult createEntities(std::span<const SceneEntityDocument> entities, AssetManager& assets,
                                              const SceneDocumentOptions& options, Scene& destination);
};
