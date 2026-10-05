# Assets

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/assets/Assets.*`

Defines renderer-independent CPU asset types and opaque typed handles:

- `ModelAsset` is a flat list of named nodes with local transforms, parent
  indices, and optional mesh handles.
- `MeshAsset` owns vertices, indices, and material-bearing draw primitives.
- `MaterialAsset` holds glTF metallic-roughness factors and texture handles.
- `TextureAsset` stores width, height, a `TextureUsage` (color, normal, or
  data) derived from referencing material slots, and either an RGBA8 base level
  or a complete BC7/BC5 mip chain.

`AssetManager` owns these values in recyclable generational slots. It normalizes
model paths, caches repeated loads, and resolves handles without exposing its
vectors. Scene sessions, editor operations, audio voices, and renderer frame
slots retain explicit leases. Deterministic collection reclaims unreferenced
assets under configurable CPU and decoded-audio budgets; recycled slots advance
their generation so stale handles cannot alias new content. Renderer frame
leases defer GPU cache pruning until submitted work has retired.

At runtime the engine collects after every scene change and whenever the
application asks (`ApplicationControl::requestAssetCollection`, or
`GameServices::requestAssetCollection` from gameplay).
`DeferredAssetCollection` (`src/engine/application/`) runs the collection
`Renderer::framesInFlight` frames after the latest request, when the frame
slots that drew the old content have dropped their leases. It evicts every
unleased CPU asset and collects decoded audio down to its budget only, since
idle emitters and gameplay code hold clip handles without leases. Anything
gameplay keeps across a collection without it being in the scene (a
preloaded model, for example) needs an `AssetLease`. A roguelike floor
teardown destroys the floor's entities, then calls
`services.requestAssetCollection()`, so floors with different tilesets do not
accumulate.

### Runtime content

`createMesh`, `createMaterial` and `createTexture` add assets built in code
(procedural walls and floors, merged room geometry, tinted materials). Each
validates its input and returns `AssetLoadResult` with a descriptive error,
adding nothing, when it fails:

- a mesh needs vertices, indices and primitives; every index must address a
  vertex, every primitive range (and simplified LOD range) must be whole
  triangles inside the index buffer, every primitive needs a live material of
  the same manager, vertices must be finite, and morph deltas must be one per
  vertex. Bounds and the local box are recomputed.
- a material's factors must be finite and its texture slots empty or live.
- a texture is RGBA8 with `width * height * 4` bytes (or half floats for an
  environment map), or a valid block-compressed mip chain, at most 16384 on a
  side.

Runtime assets get fresh generational slots like imported ones, with no
source path: no load call returns them, `meshSource`/`textureSource` are empty,
diagnostics show `runtime:<debug name>`, and a scene that references a runtime
mesh cannot be saved (the codec reports the "unknown runtime handle"). They
are leased and collected exactly like imports, so a creator leases what it
keeps until the scene that holds it is leased. They are immutable: to change
one, create its replacement, point the components at the new handle (marking
them renderable- or physics-dirty), and let the old asset be collected. The
renderer and physics key their caches by handle and generation, so a
replacement is uploaded fresh and the old one's GPU memory and Jolt shapes are
released after collection.

### Material files

`MaterialAssetFile.*` reads and writes `.zmat` files: one standalone
metallic-roughness material as JSON (`version`, `base_color`, `emissive`,
`emissive_strength`, `metallic`, `roughness`, `normal_scale`,
`occlusion_strength`, `alpha_mode`, `alpha_cutoff`, `double_sided`, and
optional `base_color_texture`, `normal_texture`, `metallic_roughness_texture`,
`occlusion_texture`, and `emissive_texture` references). A texture reference
is `{"guid", "path"}` with the path relative to the material file's folder,
so a folder of a material and its images moves as a whole; the GUID is for
editors. Missing values keep `MaterialFile`'s defaults, a newer version or
negative factor is rejected, and `writeMaterialFile` writes atomically.

`AssetManager::loadMaterial(path)` loads one, caching by path like models:
base color and emissive images load as color, normal maps with
`TextureUsage::Normal`, and metallic-roughness and occlusion maps as linear
`Data` (each standalone RGBA8, cached per path and usage), and a slot whose
image fails to load stays empty with a warning. `materialSource` names the
file. `reloadMaterial` reads the file into a new handle and updates the
cache; the old material stays valid for its users until they are re-pointed
and `collectUnused` frees it (eviction also drops it from the cache).
`leaseSceneAssets` leases material override materials (with their textures)
and particle sprite textures along with the other scene assets.

## `src/engine/assets/MeshBuilder.*`

Assembles a `MeshAsset` in code for `createMesh`: primitives by material,
vertices, triangles and quads (counter-clockwise from the front), rectangles,
boxes, and indexed arrays. `build` generates normals for vertices given none,
tangents from the texture coordinates, and bounds, then (by default) runs
`optimizeMesh` like an import, so large primitives get LOD ranges. Mistakes
such as an index past the vertices are reported by `build`. It also hosts the
normal and tangent generators the glTF importer uses.
## `src/engine/assets/AssetStreaming.*`

Owns deterministic derived-asset residency policy without importing Vulkan.
It tracks independent CPU/GPU generations, ranks requests by visibility,
distance, gameplay importance, and deadline, and emits bounded decode and
upload work lists. Texture mip ranges and mesh chunks are independent segments.
Ordered pressure handling discards temporary caches, removes optional distant
mips, retires unused generations after a completion serial, then reports a hard
budget failure if live content still cannot fit.

## `src/engine/assets/TextureMipStreaming.*`

Renderer-independent mip-range streaming policy. It picks the resident tail a
streamed texture starts with, turns object bounds into projected-visibility
priorities, and plans each frame's contiguous mip-range uploads within a byte
budget (one level per texture per round, most visible first, with a lone
oversized level always allowed so streaming progresses). The mesh pass owns the
GPU side: it uploads planned ranges from the cooked mip data and republishes
the texture's views over the wider range.

## `src/engine/assets/PackedVertexStreams.*`

Renderer-independent quantization of `Vertex` into split GPU streams:
float3 positions, octahedral snorm16 normal/tangent (bitangent sign folded into
the tangent) with float2 UVs, and uint16 joints with unorm8 weights renormalized
to sum to 255. The skin stream exists only for meshes with weights. Asset GPU
byte estimates use the same strides.

## `src/engine/assets/ModelImporter.*`

Uses cgltf to parse `.gltf` and `.glb`, load buffers, validate the document, and
convert supported nodes, meshes, primitives, materials, textures, and transforms
into Zenith types. It handles indexed and non-indexed triangle geometry and
generates normals or tangent space when absent. Non-triangle primitives are
skipped. Morph deltas and animation weight channels are retained, while
compressed paths report the missing optional decoder by name. Parsing,
resource-limit, and validation failures return descriptive errors. Each
imported mesh then passes through `MeshOptimization`. Materials also take the
`KHR_materials_clearcoat`, `sheen`, `specular`, `transmission`, `volume`, and
`ior` extensions (see `docs/RENDERER_VALIDATION.md`), whose texture slots join
the color/normal/data usage classification.
`readModelOutline` reads only a model's glTF JSON (no buffers or images) and
returns its meshes by glTF index (the `loadMesh` index) with their names,
triangle primitives in `MeshAsset::primitives` order and the material each
uses, whether they are skinned, and their morph target counts, plus the
material names, so tools can name a model's contents cheaply.

## `src/engine/assets/MeshOptimization.*`

Uses meshoptimizer at import time: vertex-cache and overdraw triangle ordering,
vertex-fetch reordering that moves every per-vertex stream (including morph
deltas) together, and up to three simplified LOD index ranges per primitive.
LODs reuse the full-detail vertices, keep primitive borders locked, and record
an object-space geometric error. Skinned and morphed primitives keep full
detail only.

## `src/engine/assets/ImageDecoder.*`

Uses stb_image to decode conventional external or embedded images and validates
KTX2 containers. RGBA8 KTX2 has a CPU fallback and BC5/BC7 KTX2 loads its
stored mip chain; Basis data reports the optional transcoder requirement.
Radiance `.hdr` images decode to linear float texels (`TextureAsset::rgbaFloat`)
for image-based lighting. GPU format policy and upload remain renderer-owned.

## `src/engine/assets/CompressedTexture.*`

Block-compressed texture layout (mip extents and sizes), the BC5/BC7 KTX2
writer and reader, and the cooked-texture convention: a cooked entry is a
`DerivedAssetCache` file keyed by the source image bytes and
`cookedTextureRequest()` settings. `loadCookedTexture()` returns nothing for a
missing entry so importers fall back to the source image.

## `src/engine/assets/TextureCooker.*`

Offline cooking used by `zenith_asset_build` and tests. Builds a full mip chain
(sRGB-correct for color, renormalized for normals) and encodes BC7 for color and
data or BC5 for normal maps, plus a CPU block decoder for validation.

## `src/engine/assets/ProductionAssetPipeline.*`

Defines untrusted-content limits, isolated Meshopt/Draco decoder registration,
KTX2 target-format choice, and deterministic derived assets keyed by source,
settings, tool version, and target. Editor inspection and the
`zenith_asset_build` command share this implementation; the command also cooks
single textures (`cook-texture`) or a model's external textures
(`cook-model-textures`).

## `src/engine/assets/ColorLut.*`

Parses Adobe/Resolve `.cube` 3D lookup tables (size, domain, Resolve's input
range, comments) into a renderer-independent `ColorLut`, rejecting 1D tables
and malformed input with line-numbered errors. `ColorGradePass` owns the GPU
texture.

## `src/engine/assets/ContentHash.*`

Incremental 64-bit FNV-1a over bytes, text, and streamed files, with a
sixteen-digit lowercase hex form. It is the single implementation behind
`DerivedAssetCache` keys, whose byte sequence (source, settings, tool version,
target, no separators) must not change without invalidating cached entries.

## `src/engine/assets/AssetGuid.*` and `AssetMeta.*`

`AssetGuid` is a 128-bit identifier written as 32 lowercase hex digits; the
all-zero value is null. `AssetGuidGenerator` is owned by its user and seeds
from `std::random_device` (or a fixed seed for tests). `AssetMeta` is the
`<file>.meta` JSON sidecar (`version`, `guid`, importer-defined
`importSettings` object), parsed strictly and written atomically through a
`.tmp` replace.

## `src/engine/assets/AssetDatabase.*`

Tracks every regular file under a project root by the GUID in its sidecar,
using project-relative UTF-8 forward-slash paths and path-ordered iteration.
Hidden names, `*.tmp`, sidecars, and configured names/extensions are skipped;
directories have no sidecar. `rescan()` creates missing sidecars and returns
Added/Removed/Modified/Moved changes. Files moved outside the editor keep
their GUID when the sidecar travels with them, or when exactly one vanished
asset matches their size and timestamp. A duplicated GUID stays with its
previous path (else the lexicographically first) and other copies are
reassigned. Unreadable sidecars are never overwritten. Orphan sidecars are
reported and kept or deleted per `OrphanMetaPolicy`. `move`, `rename`, and
`remove` act on files or directories and carry sidecars along. An in-memory,
GUID-keyed dependency graph answers direct and transitive dependents. It knows
nothing about scenes or GPU resources.
