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
| `CameraComponent` | Perspective projection settings and primary-camera selection. |
| `ParticleEmitterComponent` | GPU particle emitter parameters (`ParticleEmitter.*`, with `validateParticleEmitter`). |

`createEntity` always installs name, transform, and relationship components.
`setParent` updates both sides of a relationship and rejects self-parenting and
cycles. `instantiateModel` expands imported nodes into entities while preserving
their hierarchy. `destroyEntity` recursively destroys descendants.
`updateWorldTransforms` computes `world = parent.world * local` for the
branches under entities marked with `markTransformDirty` (and, in an audit
every frame in debug builds and every 300 calls otherwise, any entity whose
local transform or parent changed without being marked). Its cost follows the
number of changed entities, not the scene's size: marking is constant time and
each dirty branch is walked once per call.

`findEntity` is a constant-time lookup in an id index that `Scene` keeps
itself, since it is the only code that creates or destroys entities or writes
`SceneEntityIdComponent`; `clear`, `swap`, and `replaceWith` move the index
with the registry. Never create or destroy entities through `registry()`.

## `src/engine/reflection/*` and `src/engine/scene/ComponentTypes.*`

`Fields<T>` describes a struct's members (`FieldDescriptor`: key, kind,
optional flag, and inspector hints such as ranges, angles, and colors);
`FieldJson` reads and writes described fields generically. A `SceneEntityId`
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
emitter, decal), `AnimationComponentTypes.cpp` (animation, morph weights,
skin), `LightComponentTypes.cpp` (directional, point and spot lights,
environment light, volumetric fog, reflection probe, irradiance volume),
`PhysicsComponentTypes.cpp` (collider, rigid body), plus
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

## `src/engine/scene/EntityReferenceRemap.*`

One generic pass rewriting the entity references held by a scene document
entity's component objects when a set of entities is copied: every reflected
`FieldKind::EntityReference` field of the registered types (nested `Struct`
fields included, addressed by JSON pointer) plus the custom-codec
`model_node`/`skin` `instance_root`. Prefab capture, instantiate, refresh, and
the editor's duplicate/paste (`SceneFragment`) all use it.

## `src/engine/jobs/JobSystem.*` and `scene/AdditiveSceneManager.*`

`JobSystem` runs bounded prioritized work with cancellation and filtered
completion queues. `SceneDocument` separates background-safe parsing from
main-thread ECS and asset commits. `AdditiveSceneManager` composes those pieces
into stable scene instances with per-instance asset leases, desired-set
streaming, deterministic unload, and validated cross-instance entity
references.

## Folder contents

`src/engine/reflection/`: Plain-data field descriptors and their JSON codec; no
scene or asset knowledge (types such as `SceneEntityId` opt into a kind through
`ReflectedFieldKind`).

`src/engine/scene/`: ECS components, hierarchy, transforms, collections and
visibility (`Collections`, `SceneVisibility`), the component type registry, and
scene serialization.
