# Scene

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/scene/Scene.*`

Wraps an EnTT registry and defines the core ECS components:

| Component | Meaning |
| --- | --- |
| `NameComponent` | Human-readable entity label. |
| `TransformComponent` | Local transform relative to a parent and derived world transform. |
| `RelationshipComponent` | Parent entity plus an owned list of child entities. |
| `ModelComponent` | Records the model instantiated at a root entity. |
| `MeshRendererComponent` | Refers to CPU mesh data through a `MeshHandle`. |
| `MaterialOverrideComponent` | Per-instance tint of a mesh renderer: base color, emission, roughness and metallic factors, and an optional runtime-only replacement material (see [Graphics passes](graphics-passes.md#material-overrides)). |
| `CameraComponent` | Perspective projection settings and primary-camera selection. |
| `ParticleEmitterComponent` | GPU particle emitter parameters (`ParticleEmitter.*`, with `validateParticleEmitter`). |

`createEntity` always installs name, transform, and relationship components.
`setParent` updates both sides of a relationship and rejects self-parenting and
cycles. `instantiateModel` expands imported nodes into entities while preserving
their hierarchy. `destroyEntity` recursively destroys descendants.
`updateWorldTransforms` computes `world = parent.world * local` for the
branches under entities marked with `markTransformDirty`. Its cost follows the
number of changed entities, not the scene's size: marking is constant time and
each dirty branch is walked once per call.

### Transient entities

`TransientComponent` is a tag for generated content, such as the preview a
level generator builds for one seed in the editor. The tagged entity and its
whole subtree are transient (`Scene::isTransient`); `markTransient` tags an
entity, `transientEntities` lists every transient entity (each tagged subtree
parents first), and `destroyTransient` destroys them all and returns how many.
`duplicateSubtree` copies the tag.

Transient entities never reach a scene document: `SceneDocument::serialize`
and `save` leave them out, `serializeEntities` reports them as absent, and
`applyChanges` lets a removal take its transient children with it (documents
never list them). A saved entity's reference to a transient one is written as
none rather than as a dangling id: every reflected entity reference field is
nulled in the document (the scene keeps its value), and `serialize` and `save`
return a warning per reference in `SceneDocumentResult::warnings`, which the
editor lists in Info when saving. Instance roots of model nodes and skins are
never nulled; they always lie in their own (transient) subtree. Save games
skip transient entities too, so a game that tags its generated level must
regenerate it from the saved seed. `tests/engine/TransientEntityTests.cpp`
covers the tag, the helpers, and the documents.

### Transforms from gameplay code

EnTT stays public, but `Scene`'s transform helpers are the supported gameplay
API for moving entities (`Scene.hpp`, implemented in `SceneTransforms.cpp`):

- Setters write `TransformComponent::local` and mark the entity dirty:
  `setLocalTransform`, `setPosition` (only the translation column changes),
  `setRotation` and `setScale` (rebuilt from a TRS decomposition, which drops
  shear), `translate` (an offset in the parent's space), and
  `setWorldTransform` (converted to parent space through the parent's
  resolved world transform).
- Readers: `localTransform`, `localTrs`, `localPosition`, `localRotation`,
  `localScale`, and `worldTransform`/`worldPosition`. The world readers
  multiply the local transforms of the entity and its ancestors on demand, so
  they are correct straight after a setter, before `updateWorldTransforms`,
  and work on a `const Scene`; they cost one matrix product per ancestor.
  `TransformComponent::world` is only current after `updateWorldTransforms`.
- `TransformTrs.hpp` has the decomposition (`decomposeTrs`, `composeTrs`): the
  translation column exactly, each basis column's length as scale (a mirrored
  basis negates X), and the Gram-Schmidt orthonormalized basis as rotation.

Writing `TransformComponent::local` through `registry()` is allowed for
engine internals and bulk code, but it must be followed by
`markTransformDirty(entity)`. A missed mark is caught by an audit that
`updateWorldTransforms` runs on its first call and then every
`transformAuditInterval()` calls (`defaultTransformAuditInterval`, 60, in
Debug and Release alike; `setTransformAuditInterval(0)` turns it off). The
audit runs after the marked branches are updated, so anything still out of
date was changed without a mark: it counts it in
`lastTransformUpdateStats().unmarked`, logs a warning naming the entity once
per entity on the scene's log channel (`setLogChannel`; the engine passes its
logger under the `scene` category, otherwise stderr), and updates the entity.
A missed mark therefore shows as a warning and a delay of up to one interval
in every build configuration, rather than being silently repaired in Debug.

`findEntity` is a constant-time lookup in an id index that `Scene` keeps
itself, since it is the only code that creates or destroys entities or writes
`SceneEntityIdComponent`; `clear`, `swap`, and `replaceWith` move the index
with the registry. Never create or destroy entities through `registry()`.

## `src/engine/reflection/*` and `src/engine/scene/ComponentTypes.*`

`Fields<T>` describes a struct's members (`FieldDescriptor`: key, kind,
optional flag, runtime-state flag, and inspector hints such as ranges,
angles, and colors); `FieldJson` reads and writes described fields
generically. A field marked `.runtimeState()` (health left, a door's
progress) is left out of documents whose `SceneDocumentOptions::purpose` is
`Scene` (scene files, prefabs, undo snapshots) and written for `SaveGame`;
readers treat it as optional either way. Custom codecs see the purpose in
their save context. A `SceneEntityId`
member is an entity reference field (`FieldKind::EntityReference`), written
as the entity's id or `null` like a scene document's `parent`; an id whose
entity no longer exists is kept unchanged. Reflection does not know the id
type: `scene/SceneEntityId.hpp` opts it in through the `ReflectedFieldKind`
trait, so `reflection/` stays free of scene code. A `ComponentType`
pairs a component's stable document key with its fields or a custom codec,
validation, sanitizing, texture bindings, and schema migrations, and a
`ComponentTypeRegistry` holds the types a document understands.
`BuiltInComponentTypes.cpp` registers the engine's components in a fixed
order (the order documents are read in and the editor lists them; a test pins
it). Their types are built in one family file per domain:
`RenderComponentTypes.cpp` (model, model node, mesh renderer, camera, particle
emitter, decal), `AnimationComponentTypes.cpp` (animation, animator, morph
weights, skin), `LightComponentTypes.cpp` (directional, point and spot lights,
environment light, volumetric fog, reflection probe, irradiance volume),
`PhysicsComponentTypes.cpp` (collider, rigid body, character controller),
`GameplayComponentTypes.cpp` (first-person camera), plus
`CollectionComponentTypes.cpp` and `PrefabComponentTypes.cpp`; shared codec
helpers are in the internal `ComponentTypeHelpers.hpp`. Add a new built-in
type to the family file for its domain and append it to the list in
`BuiltInComponentTypes.cpp`;
`SceneDocumentOptions::componentTypes` lets a game pass a registry that adds
its own. `SceneDocument` saves and loads every registered component, and keeps
components with unknown keys verbatim in `PreservedComponentsComponent`.
`SceneDocument::diff` lists the entities whose document form differs between
two serialized documents (plus the collection definitions when they changed,
as `SceneChanges`), and `SceneDocument::applyChanges` patches those entities
and collections in place toward either side atomically, leaving every other
entity, its EnTT handle, and its child order untouched.
`SceneDocument::serializeEntities` writes chosen entities in exactly the form
a `SceneEntityChange` holds, so a caller that knows which entities an edit
touches can build `SceneChanges` without serializing the whole scene.
`SceneAssetReferences.*` converts project-relative asset references for codecs.

Scene documents are versioned (`SceneDocument::currentVersion`, now 3).
`parse` runs ordered document-level migrations (`fromVersion` to
`fromVersion + 1`, on the raw JSON) before validating, so older files load
unchanged in behavior and save as the current version; a newer version is
rejected with a message naming the supported one. Version 1 wrote asset
references as bare path strings (`environment_asset`, `texture_asset`,
`albedo_asset`, `normal_asset`, `bake_asset`) or `{"path": ...}` objects
(model, model node, skin, mesh renderer, triangle-mesh collider). Version 2
writes every reference as an object holding the project-relative `path` and,
when `SceneDocumentOptions::assetDatabase` knows the asset, its `guid`; an
empty string still means "no asset". Loading resolves a GUID through the
database first, so a scene keeps working after its assets are renamed or
moved, and falls back to the path without a database or for unknown GUIDs.
Version 3 adds a top-level `collections` array (see Collections below);
version 2 documents migrate with none. Component structs keep path strings at
runtime; GUIDs live only at the document boundary. Texture paths (`ComponentTypeBuilder::texture`) and other
`assetReference` fields are converted after component migrations run. Inside
component JSON the key `path` is reserved for asset references, which lets
`relocateSceneAssetReferences` rewrite references to moved files generically.
Prefab entities use the same entity form and component codecs, so their asset
references carry GUIDs too; nested-prefab sources are still paths relative to
the containing prefab.

## `src/engine/scene/SaveGame.*`

`SaveGameStore` keeps save slots (`<slot>.zsave`, names of letters, digits,
`-` and `_`) in a directory. `save` serializes the running scene for a save
game and records only how it differs from its baseline scene document
(`SceneDocument::diff`, keeping each changed entity's new state), with a
title, a timestamp, and the baseline's path (relative to the store's content
root when inside it). `load` reads the baseline again and applies the changes
(`applyChanges`, forward), then replaces the destination scene, so entities
the game never touched follow later edits of the level. Writes are atomic; a
missing or corrupted slot loads its previous save from the `.bak` backup.
`list` returns the slots newest first. Games reach the session's store
through `GameServices::saveGames` and `saveGame`; `ProjectApplication` puts
it under `userDataDirectory("Zenith", <project name>)/saves`, and the editor's
Play under the project's `.zenith/saves`.

A slot can hold a game's own data instead of a scene. This suits games that
rebuild their scene, such as a roguelike that regenerates its floor from a
seed.

- `saveData(slot, data, Fields<Data>, SaveDataFormat, title)` writes a
  reflected struct under a `kind` (for example `run`) and the format's own
  `version`. `RandomStreamsSnapshot` (see [Gameplay](gameplay.md)) embeds a
  session's random streams in it.
- `loadData` reads it into a default-constructed `Data`. If the saved
  version is older, it first runs the format's `migrate`; a newer version is
  refused. On failure the target is unchanged.
- Every slot records its kind (`SaveGameInfo::kind`, `scene` for scene
  saves and for saves made before kinds existed). A data slot does not load
  as a scene, nor a scene as data, nor one kind as another.

`saveProfile` and `loadProfile` keep data across runs (unlocks, statistics)
in `<name>.zprofile`. It is a versioned JSON file (`VersionedJson`) beside
the slots, written atomically with a backup. `list` does not show it and
`remove` never touches it, so a game that deletes its run slot on death
keeps the profile. A missing profile loads as success and keeps the
caller's defaults.

## `src/engine/scene/Collections.*`, `SceneVisibility.*`, `CollectionComponentTypes.cpp`

Blender-style collections: named, nested groups of entities kept in
`Scene::collections()` (`SceneCollections`, ascending stable ids) and
replaced as a whole with `Scene::setCollections`, which invalidates render and
light caches. Collections are independent of the transform hierarchy, and an
entity joins any number of them through `CollectionMembershipComponent`
(`collection_membership`, sorted ids); an entity in none belongs to the
implicit scene root. `linkToCollection`, `unlinkFromCollection`, and
`deleteCollection` (contents move to the parent collection, instancers lose
their component) keep membership and definitions consistent. Documents store
each collection as `{id, name, parent, visible, selectable, renderable,
instance_offset}`; every membership and `collection_instance` reference must
name a defined collection, or loading and patching fail.

Visibility (`SceneVisibility`) combines the entity's `VisibilityComponent`
(`visibility`: `hidden`, `selectable`, `renderable`) with its collections; an
entity in collections needs at least one of them (and all of that one's
enclosing collections) to allow a flag. `SceneVisibilityMode::Runtime`, the
renderer's default, honours only `renderable`, so games and Play mode draw
editor-hidden entities; `Editor` also applies the viewport hide (entity
`hidden`, collection `visible`), and picking requires `selectable`. An entity
with `CollectionInstanceComponent` (`collection_instance`) draws the
renderable members of a collection and its nested collections at its
transform, minus the collection's `instance_offset`; `expandCollectionInstances`
does the CPU expansion (nested instances to depth 8, cycles skipped) for the
renderer and the editor picker. Instances ignore the viewport hide, so the
source collection can be hidden while its instances show.

## `src/engine/scene/Prefab.*`, `PrefabSystem.cpp`, `PrefabComponentTypes.cpp`

`Prefab.hpp` declares the whole prefab API. `Prefab.cpp` reads and writes the
documents. `PrefabSystem` is split by domain:
- `PrefabSystem.cpp`: instantiate, refresh and capture;
- `PrefabOverrides.cpp`: comparison and applying overrides, and recording
  removals and moves;
- `PrefabNesting.cpp`: nested instances;
- `PrefabIdRemap.cpp`: id remapping;
- `PrefabQueries.cpp`: instance queries.

They share helpers through the internal `PrefabSystemInternal.hpp`.

Prefab documents (version 2, `.prefab` files under the asset root) list one
root entity and its descendants in the scene document entity form (`id`,
`name`, `parent`, `local_transform`, and `components` keyed by registered
component type) with prefab-local ids, plus nested prefab records
(`{id, source, parent, overrides}`). Components are read and written by the
`ComponentTypeRegistry`, so module components work and unknown keys survive a
load as preserved components. Version 1 documents (`localTransform`,
`modelAsset`, `pointLight`/`camera`, whole-component overrides) migrate on
load; a model component whose node entities a prefab does not list gets its
node hierarchy instantiated. An optional `metadata` object holds
`display_name`: the name the prefab was created from (file names are reduced
to portable ASCII), shown by Add > Prefab and the instance header and kept by
apply.

Entity references in prefab components hold prefab-local ids, or, for an
entity of a nested instance, `prefabNestedReference(record, local id)` (bit
62, the record id in bits 32-61, the nested prefab's local id below), so
references into nested prefabs survive create, apply, and instantiate (one
level deep); instantiate and refresh resolve them once the nested instances
exist. References to anything else are cleared with a diagnostic.

Overrides are `{entity, component, field, value}` with `field` an RFC 6901
pointer into the component's document object (empty for a whole component;
value null removes it), or, with an empty component, `/name`,
`/local_transform`, `/parent` (a prefab-local id: moved within the instance;
or a scene id with bit 63 set: moved under an entity added to the instance in
the scene, which falls back to the prefab parent when that entity is gone),
or `/removed` (value true: deleted from the instance, recorded by
`recordPrefabEntityRemoval` from the editor's delete commands).
`{nested, component: "", field: "/removed", value: true}` records a nested
instance deleted from (or moved out of) its containing instance, by nested
record id; refresh skips the record, apply drops it from the prefab and from
the other instances. Moving a prefab entity out of its instance (outliner
drop, `recordPrefabEntityMove`) records its removal and unlinks the moved
subtree, which stays in the scene as plain objects (nested instances in it
become placed instances). In instance overrides, references to entities
outside the instance are scene ids with bit 63 set; in a prefab file a nested
record's overrides use bit 63 for the containing prefab's local ids instead,
and capture drops any other scene reference, so prefab files never hold scene
ids. Collection
membership and collection instances are scene-local: never stored in a
prefab, kept on instance entities through refreshes, never overrides.
Instances persist their link: the root holds `prefab_instance`
(the prefab as a `{guid, path}` asset reference, the overrides as of the last
prefab operation, and the nested record it came from) and every entity created
from the prefab holds `prefab_entity` (its prefab-local id); both are internal
types registered from `PrefabComponentTypes.cpp`. An instance is its root and
descendants up to the next instance root, so nested prefabs instantiate as
separate instances. `PrefabSystem` instantiates (validating the whole nested
graph in a staging scene first and rejecting cycles), refreshes an instance
from its prefab through `SceneDocument::applyChanges` (stable ids kept, added
objects kept or removed), reports live overrides by diffing the instance's
serialized entities against the prefab's normalized entities, and captures a
scene subtree as a document. It is runtime code, so primary and additive
scenes share definitions without editor state. Entity references inside
component JSON are remapped by `EntityReferenceRemap` (below): capture clears
references outside the prefab with a diagnostic.

A failure after the staging check (a nested instance that no longer matches
its prefab) puts the scene back. Only the instance's own subtree is saved
first, and the entities the change created are found through
`Scene::nextEntityId`, so instantiating and refreshing cost what the prefab
costs, whatever the size of the scene.

## `src/engine/scene/EntityReferenceRemap.*`

One generic pass rewriting the entity references held by a scene document
entity's component objects when a set of entities is copied: every reflected
`FieldKind::EntityReference` field of the registered types (nested `Struct`
fields and `List` elements included, addressed by JSON pointer) plus the custom-codec
`model_node`/`skin` `instance_root`. Prefab capture, instantiate, refresh, and
the editor's duplicate/paste (`SceneFragment`) all use it.

## `src/engine/jobs/JobSystem.*` and `scene/AdditiveSceneManager.*`

`JobSystem` runs bounded prioritized work with cancellation and filtered
completion queues. `SceneDocument` separates background-safe parsing from
main-thread ECS and asset commits. `AdditiveSceneManager` composes those pieces
into stable scene instances with per-instance asset leases, desired-set
streaming, deterministic unload, and validated cross-instance entity
references.

## `src/engine/scene/Tags.*`

`TagsComponent` (key `tags`, category Gameplay) lists names game code finds
entities by: `hasTag(scene, entity, tag)` and `findEntitiesWithTag(scene,
tag)`, in scene id order. It saves as `{"tags": [...]}` through a custom
codec, since reflection has no string lists, and rejects empty names. The
project's manifest lists the tags the editor's picker offers, but any name
works at runtime.

## Folder contents

`src/engine/reflection/`: Plain-data field descriptors and their JSON codec; no
scene or asset knowledge (types such as `SceneEntityId` opt into a kind through
`ReflectedFieldKind`). `Fields::list` describes a `std::vector` of nested
structs (`FieldKind::List`), written as an array of objects; reading one
replaces the list, each element starting from its defaults. The inspector
edits a list on one selected object (add, remove, and each element's
fields); several selected objects show only their counts. There are no lists
of plain values (strings, numbers) yet.

`src/engine/scene/`: ECS components, hierarchy, transforms, collections and
visibility (`Collections`, `SceneVisibility`), the component type registry, and
scene serialization.
