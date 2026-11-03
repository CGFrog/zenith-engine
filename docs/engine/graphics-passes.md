# Graphics scene passes

Part of the [repository guide](../REPOSITORY_GUIDE.md).

The file sections come first. The feature contracts (settings, fallbacks, GPU
scopes, budgets, and tests) follow them:
[Ground-truth ambient occlusion](#ground-truth-ambient-occlusion),
[Single-pass depth pyramid](#single-pass-depth-pyramid),
[GPU-culled shadow cascades](#gpu-culled-shadow-cascades),
[GPU particles](#gpu-particles),
[Reflection probe blending](#reflection-probe-blending),
[Clustered decals](#clustered-decals),
[KHR material extensions](#khr-material-extensions),
[Local-light shadows](#local-light-shadows), and
[View shading modes](#view-shading-modes).

## `src/engine/graphics/passes/GpuDrivenPass.*`

Two-phase GPU frustum and occlusion culling and draw compaction into
indirect-count commands. The cull shader also picks each object's LOD batch.
Both phases' compacted draws stay valid for the rest of the frame, so the
shading pass redraws what the prepass culled. The caller owns render passes,
the depth pyramid, and the scene-depth transitions around the pyramid build.
Frustum-only views (the directional shadow cascades) cull into their own
output regions without touching the camera phases or visibility history.

## `src/engine/graphics/GpuCullLayout.*`

Device-free contracts of GPU culling: cull phases and modes, the region-major
layout of the cull output buffers (camera phases first, then frustum views),
and the clip-volume sphere test that `gpu_visibility.comp` mirrors and the CPU
shadow-cascade cull shares.

## `src/engine/graphics/passes/DepthPyramid.*`

Builds the hierarchical depth of the scene view: one RG32F image in `GENERAL`
layout, red = farthest (min, reversed Z) and green = closest (max) depth, level
0 a copy of the depth over the render extent. `DepthPyramidLayout.hpp` holds
the CPU-testable level-extent and texel-coverage rules the shaders mirror.

## `src/engine/graphics/passes/AmbientOcclusionPass.*`

GTAO after the depth prepass: `gtao.comp` writes raw visibility and view depth
(one shared RG16F image); at full resolution `gtao_denoise.comp` blurs it
depth-aware, at half resolution `gtao_upsample.comp` denoises and upsamples it,
into an R8 render-resolution image per frame slot that `mesh.frag` reads at
camera binding 13 for indirect diffuse and specular occlusion. Cleared to 1
when occlusion is off or the prepass did not run.

`gtao.comp` runs right after the prepass and pyramid and integrates
cosine-weighted horizon visibility per slice from the prepass normals and the
depth pyramid (view positions come from the inverse of the jittered
projection). `ambient_occlusion::qualityProfile` maps `AmbientOcclusionQuality`
to a resolution and sampling pattern: Full traces every render pixel with 3
slices x 4 steps against pyramid level 0 and blurs the result with
`gtao_denoise.comp` (depth-aware 5x5, covering the 4x4 noise tile);
HalfResolution traces a quarter of the pixels with 2 slices x 4 steps against
level 1 (checkerboarded farthest/closest depth), and `gtao_upsample.comp`
denoises and upsamples to the render resolution in one pass, averaging the 4x4
raw texels around each pixel weighted by view-depth similarity. The clamp and
occlusion power follow the average so few-slice estimates are not biased dark.
The R8 result (one render-resolution image per frame slot, kept in `GENERAL`)
is sampled by `mesh.frag` with `ambient_occlusion::occlusionUv` (scale in
`environmentParams.w`, 1 for every quality). Its minimum with material
occlusion attenuates indirect diffuse, and Lagarde's specular occlusion
attenuates indirect specular; blended surfaces skip it. With TAA the noise
rotates per frame and the temporal resolve supplies stability. When the
prepass is off (its normals are missing) or occlusion is Off, the image is
cleared to 1 and no dispatch runs.

## `src/engine/graphics/AmbientOcclusion.*`

CPU mirror of the GTAO integration, per-quality sampling profiles, horizon
sampling of depth-pyramid levels, view-position reconstruction from reversed-Z
depth, occlusion-texel mapping under dynamic resolution, specular occlusion,
and denoise and upsample weights; `tests/engine/AmbientOcclusionTests.cpp`
covers it.

## `src/engine/graphics/MeshLodSelection.*`

The CPU definition of LOD selection: the coarsest level whose geometric error,
projected from the nearest point of the bounding sphere, stays within
`RenderSettings::lodErrorThresholdPixels`. `gpu_visibility.comp` mirrors it.

## `src/engine/graphics/MeshPassFramePlan.*`

The per-frame mesh-pass decisions that change the render graph's passes,
computed by the pure `planMeshPassFrame()` from validated settings,
capabilities, and the diagnostic overrides read once at renderer startup.
`MeshPass::record` and `Renderer::updateRenderGraphPlan` consume the same
plan; `tests/engine/MeshPassFramePlanTests.cpp` covers it. Each render view has
its own plan; only the default view may plan GPU-driven submission.

## `src/engine/graphics/ViewShading.*`

View shading modes (Rendered, Material Preview, Solid, Wireframe) as pure
functions: the Wireframe capability fallback, the reduced frame settings each
mode records with (Wireframe: edges only, through every surface, with no
prepass or occlusion culling), which mesh pipeline variant it needs, and the
camera-relative studio light rig. `ScopedViewShadingSettings` applies the
frame settings while `Renderer::recordFrame` records. The modes other than
Rendered do not add a recording path: `Renderer::recordFrame` swaps in the
mode's reduced settings for the frame, so the plan, the graph model and the
passes all see the same feature set; the mesh pass only selects a pipeline
variant (clay surfaces, the LINE overlay), lights the camera constants with a
studio rig, and draws Wireframe's edges, and only those, through every
surface. The contract is in [View shading modes](#view-shading-modes);
`tests/engine/ViewShadingTests.cpp` covers it.

## `src/engine/graphics/passes/ClusteredLightingPass.*`

Owns the per-frame local-light buffer and the cluster light lists. Each frame
`light_clusters.comp` assigns lights to a 16x9 screen grid times 24
exponential view-depth slices before the scene pass; `Lighting.*` holds the
CPU mirror of the cluster math used by tests. Point and spot lights are
gathered by stable entity ID, capped at the configured `maximumLocalLights`
(nearest to the camera kept), and uploaded to a per-frame storage buffer; the
depth slices are exponential, reversed-Z-aware, and cover the internal render
extent, and `mesh.frag` loops only its cluster's list. Older settings that
chose 32- or 64-light tiles load as clustered.

## `src/engine/graphics/LocalShadows.*` and `src/engine/graphics/passes/LocalShadowPass.*`

Local-light shadows. Point and spot lights opt in with `castsShadows` (plus
resolution and priority hints) and share one depth atlas sized by
`LocalShadowQuality`. `LocalShadows.*` is the CPU policy: atlas requests (spot
lights before point lights, in a camera-independent order so placements stay
put), per-tile projections (one perspective tile per spot, six cube faces per
point light chosen by major axis), and `LocalShadowCache`, which keeps tiles
whose light and nearby casters did not change and schedules at most the
quality's per-frame tile budget for re-rendering, unrendered lights first and
then the nearest. `LocalShadowPass` owns the atlas, its comparison sampler
(camera binding 14), the per-frame tile records (binding 15), and the
depth-only `local_shadow.vert` pipeline; `MeshPass` supplies each tile's
casters. A shadowed light's `GpuLocalLight::spotCone.z` names its first tile,
and `mesh.frag` filters four bilinear comparisons with a slope-scaled normal
offset.

## `src/engine/graphics/passes/VolumetricFogPass.*`

Froxel volumetric fog for scenes with a `VolumetricFogComponent` when
`RenderSettings::volumetricFog` is not Off. After light clustering,
`volumetric_fog_inject.comp` evaluates height fog (with optional noise) lit by
the cascade-shadowed sun, the froxel's cluster light list, and ambient light,
blending it with the reprojected previous frame (Medium and High). Ambient comes
from the environment's SH, or inside a bound irradiance volume from its probes'
visibility-weighted mean radiance (`irradiance_volume::sampleMeanRadiance`), so
fog in enclosed spaces is not lit like open sky;
`volumetric_fog_integrate.comp` accumulates it front to back. `mesh.frag` and
`skybox.frag` composite the integrated RGBA16F volume at camera binding 18.

Fog is authored per scene by the primary `VolumetricFogComponent` (density,
height falloff, albedo, Henyey-Greenstein anisotropy, noise) because the
medium is content, like lights; its cost is a project render setting,
`VolumetricFogQuality` (Off, Low, Medium, High). The froxel grid subdivides
the cluster grid: each cluster tile and depth slice splits into an integer
number of froxels with the same exponential depth law, so froxel `(x, y, z)`
reads the light list of cluster `(x / sub.x, y / sub.y, z / sub.z)` and the
volume ends at the cluster grid's `sliceEndDepth`. After light clustering and
the shadow cascades, injection writes in-scattered radiance and extinction per
froxel (sun with one hardware PCF tap of its cascade, clustered local lights
unshadowed, ambient as above), jittered and blended 1:9 with the reprojected
previous frame on Medium and High. Integration then accumulates each column
front to back, slice by slice with the analytic homogeneous-slice integral.
`mesh.frag` (opaque and blended) composites `color * transmittance +
scattering`; the skybox reads the last slice. Off records no fog work and
keeps a cleared 1x1x1 volume bound. Local-light shadows in fog are a
follow-up.

## `src/engine/graphics/Decals.*` and `src/engine/graphics/passes/DecalClusterPass.*`

Clustered decals for scenes with `DecalComponent` box projectors when
`RenderSettings::decals` is On. `Decals.*` is the CPU reference: projector
construction from the entity transform and half extents (decal space is the
unit cube; the decal projects along local -Y), application order (sort order,
then stable scene id), nearest-first selection under `maximumDecals`, the decal-space
and angle/depth fade math `mesh.frag` mirrors, conservative view-space bounds,
and the cluster assignment `decal_clusters.comp` performs. `DecalClusterPass`
owns each frame's records (camera binding 19), cluster lists (binding 20) and
statistics readback; `MeshPass::prepareDecals` resolves decal textures to
bindless slots and packs the records, and `applyDecals` in `mesh.frag` blends
them before lighting.
## `src/engine/graphics/passes/ParticlePass.*`

GPU particles for scenes with `ParticleEmitterComponent`s when
`RenderSettings::particles` is not Off. After the volumetric fog it records
`particle_emit.comp` (new particles into each emitter's ring),
`particle_simulate.comp` (integration, expiry, view culling, lighting from the
shadowed sun, clustered local lights, and ambient probes, and appending visible
particles to the sort keys and indirect draw), and `particle_sort.comp`
(bitonic sort back to front). Between blended surfaces and editor outlines it
draws `particle.vert`/`particle.frag` quads in its own rendering scope with the
scene depth read-only, sampled for soft particles.

Particles are authored per entity with a `ParticleEmitterComponent` (rate,
bursts, lifetime, cone, speed, gravity, drag, size and color over life,
emissive intensity, lit or unlit, alpha or additive, world or local space,
soft distance, optional sprite); `ParticleQuality` (Off, Low, High) sets the
total particle budget the emitters share. The CPU side
(`particles::ParticleScheduler` in `Particles.hpp`) gives each emitter, in
stable scene-id order, a ring of consecutive pool slots and decides each
frame's spawn count, so the layout, spawn ordinals, and every random value
(hashed from the emitter id and ordinal on the GPU) are reproducible under a
fixed frame time. Lit particles are lit at their centers (sun with one
shadow-cascade tap, clustered local lights, ambient from the environment SH or
the irradiance volume). `particle_sort.comp` runs a bitonic network
(shared-memory blocks of 1024 keys plus global merge steps). After blended
surfaces the scene pass ends; `particle.vert` pulls one camera-facing quad per
sorted key and `particle.frag` writes premultiplied HDR color (additive
particles write zero coverage), fades against the opaque depth, which is bound
read-only and sampled at the same time (soft particles), composites the
volumetric fog in front of the particle, and raises the motion target's
reactive channel by maximum blending so the temporal resolve trusts history
less under particles. Off records no particle work.

## `src/engine/graphics/Particles.*`

CPU side of the particle system: quality budgets, the emitter scheduler (pool
layout and deterministic spawn counts), GPU struct layouts mirrored by the
shaders, emitter packing, and the bitonic sort plan with a CPU reference of
the network; `tests/engine/ParticleTests.cpp` covers it with the emitter
validation and scene round trip of `engine/scene/ParticleEmitter.*`. Emitters
beyond `particles::maximumEmitters` (64, the highest scene ids) are not
simulated; `ParticleFramePlan::droppedEmitters` counts them, `MeshPass` logs
the overflow once per episode, and `RenderStats::droppedParticleEmitters`
reports it. An emitter fires its first burst when it starts; changing its runtime-only
`restartGeneration` starts it again without a pool reset (pooled effects use
it).

The pool layout is stable: each emitter keeps its slot range, its GPU emitter
table entry (which its particles name), its spawn accumulator and burst timer,
and its live particles while its id, capacity, and simulation space stay the
same. Adding an emitter places it in the first free range that fits and the
lowest free table entry; removing one frees its range. The plan lists only
those slots in `ParticleFramePlan::clearRanges`, which `ParticlePass` zeroes
before emitting, so an enemy spawning with a torch no longer restarts every
fire on the floor. When fragmentation leaves no free range large enough, or
the budget or scene content generation changes, the scheduler compacts:
emitters are packed in id order, everything restarts, and the pool is reset.

## `src/engine/graphics/VolumetricFog.*`

CPU mirror of the froxel layout (an integer subdivision of the cluster grid
with the same exponential depth slicing), the integrated-volume depth lookup,
front-to-back integration, Henyey-Greenstein phase, and height-fog density;
`tests/engine/VolumetricFogTests.cpp` covers it.

## `src/engine/graphics/passes/EnvironmentPass.*`

Split-sum image-based lighting. Per environment source (an equirectangular
map, or the analytic sky when none is loaded) it precomputes on the GPU, once
and cached: an RGBA16F radiance cube with mips, a GGX-prefiltered specular cube,
and order-2 spherical-harmonic irradiance; a BRDF table is built once. It binds
these into camera-set bindings 5-7 for the primary environment (and, with
`CameraOnly` probe blending, the dominant reflection probe), copies resident
probes into the per-pixel probe cube-map array (binding 16) with `PerPixel`
blending, and draws the skybox behind opaque geometry.

Precomputation is recorded into the frame when an environment source first
appears, cached by source, and evicted data retires through the frame-serial
release queue. Scenes without an environment map use an analytic sky built
from the environment colors, so lighting and the skybox stay consistent; the
lowest quality keeps hemispherical sky/ground lighting and the flat clear
color.

## `src/engine/graphics/EnvironmentLighting.*`

CPU reference for the IBL conventions (cube faces, equirectangular mapping,
analytic sky, SH projection, split-sum BRDF, sheen albedo) that the `env_*.comp`
shaders mirror, plus the content-hashed CPU environment averaging used by editor tools.

## `src/engine/graphics/passes/MeshPass.*`

Finds the primary camera, updates per-frame camera data, lazily uploads CPU
assets to renderer-owned GPU caches, and publishes bindless material records and
texture slots. It first
renders a directional shadow map, then (when `RenderSettings::depthPrepass` is
on) opaque depth and view-space normals plus the depth pyramid, and then the
scene into a floating-point HDR target using Cook-Torrance GGX lighting plus
split-sum image-based (or, at the lowest environment quality, hemispherical
sky/ground) indirect lighting. Directional and environment scene components
supply lighting and
shadow settings. Fallback textures fill missing maps. A frame records the shared
stage once (packets, uploads, environment, shadows), placed by its leading view,
and then each requested render view; a view's GPU state (`ViewState`: camera
sets, clusters, fog, depth pyramid, AO, post-process targets, history) lives
from `createView` to `destroyView`.

Local point and spot lights use clustered forward shading: up to the configured
light limit (nearest first) is uploaded each frame, and mesh.frag shades only
the lights listed for its view cluster. Environment textures are prefiltered on
the GPU by `EnvironmentPass`, while reflection probes contribute through
explicit influence boxes, blend distances, and priorities. By default
(`ReflectionProbeBlending::PerPixel`) resident probes are assigned to the
light-cluster grid and blended per pixel with box projection; `CameraOnly`
(the fallback) blends the dominant probe at the camera over the primary
environment for the whole frame. Deterministic CPU probe preprocessing and
bake-file policy live in editor tooling.

The primary directional and environment light components provide direct-light,
shadow, sky, and ground settings. Material occlusion attenuates only indirect
environment lighting, leaving direct and emissive contributions unchanged.

Indirect diffuse light comes from the environment's spherical harmonics unless
`RenderSettings::indirectDiffuse` is `ProbeVolume` and a baked
`IrradianceVolumeComponent` is in the scene. The volume nearest the camera is
bound: `IrradianceVolumePass` uploads its probes (order-2 spherical harmonics of
irradiance plus octahedral depth moments, `engine/assets/IrradianceVolumeAsset.hpp`)
to camera binding 8 only when they change, and mesh.frag blends the eight
surrounding probes with trilinear, backface, and Chebyshev visibility weights,
fading to the environment across the volume's edge
(`engine/graphics/IrradianceVolume.hpp` is the CPU reference). The editor bakes
volumes on the CPU (`editor/project/IrradianceVolumeBaker.*`): rays from each
probe see the sky, emissive surfaces, and surfaces lit by the sun and local
lights with shadow rays, plus further bounces through the previous pass's
volume. Bakes are written atomically to
`<asset root>/baked/<scene>/irradiance_volume_<entity id>_<content hash>.zvol`
and referenced from the scene document; content-addressed names keep earlier
bakes available to undo. A scene whose bake file is missing still loads, with
the volume unbaked (environment lighting) until it is baked again. The inspector's "Bake Irradiance Volume" button bakes
one volume; `zenith_editor <project> --bake-irradiance-volumes` bakes every
volume in a project's startup scene without opening a window.

Per-frame capacities that a large scene can exceed degrade instead of failing
the frame. Each overflow is logged once per episode (until a frame fits again,
`engine/diagnostics/OverflowEpisode.hpp`) and counted in `RenderStats`:

- Render items beyond `maximumObjectsPerFrame` (16384, sized into the
  per-frame object buffers): `RenderItemBudget` admits whole objects in build
  order (scene renderables, then collection instance draws) and drops every
  later one, so the drawn set is a deterministic prefix that holds while the
  scene does not change (`droppedRenderItems`).
- Primitives whose material finds no slot among the `maximumMaterials` (4096)
  material records are not drawn until a slot frees (also
  `droppedRenderItems`).
- Skinned items beyond the `maximumSkinMatricesPerFrame` (65536) joint
  matrices draw unskinned, in their bind pose (`droppedSkinnedRenderItems`).
- Material overrides beyond `maximumMaterialOverrides` (1024) distinct
  material and factor pairs draw their plain material
  (`droppedMaterialOverrides`).
- Opaque surfaces spanning more than `maximumGpuDrawGroups` (64) GPU-driven
  draw groups (pipeline bucket and geometry page pairs, so a geometry-arena
  layout limit rather than a per-scene count) send the frame's default view
  through CPU submission, which has no group limit, until they fit again
  (`excessGpuDrawGroups`, the groups beyond the limit). Nothing is left out;
  the frame loses GPU culling and the GPU shadow culling that comes with it.
  The count comes from the render items before any GPU-driven work
  (`countIndirectDrawGroups`, which predicts `buildIndirectDrawPlan`'s
  groups), so the whole frame takes one path.

The geometry arena and the bindless texture table, retried every frame while
full, warn once per episode the same way. Local lights and local shadow tiles
already keep the nearest within their limits. The LOD instance capacity
(`maximumGpuDrivenInstances`) still fails the frame and logs an error each
frame it is exceeded.

### Bindless materials and vertex streams

The mesh pass maintains per-handle GPU caches. Camera buffers are per frame to
avoid CPU/GPU write races. Materials are bindless: each frame slot owns a
material-record buffer and a runtime-sized texture array whose slots are only
rewritten after that slot's previous submission retires, so no descriptor is
updated while in use. Every object record carries its material index, so draws
of one primitive batch across materials and need no per-draw binding; missing
maps use default slots. Meshes are suballocated into shared vertex/index pages
and drawn by offset. Vertices upload as quantized split streams (position,
attributes, and skin for skinned pages only); depth-style pipelines fetch only
position, skin, and, for alpha-tested shadows, UVs. Static pages bind one zero
skin influence with a dynamic stride of 0, so every mesh pipeline shares one
vertex layout.

### GPU-driven submission and LOD

GPU-driven submission is opt-in and uses two-phase occlusion culling. Objects
visible last frame are drawn first; a depth pyramid built from that depth
then tests every object, records its visibility for the next frame, and draws
the newly visible remainder. A compaction pass packs non-empty draws so each
pipeline and geometry page is one `vkCmdDrawIndexedIndirectCount`. Blended
surfaces and editor outlines stay on CPU submission within the same frame.

Imported primitives carry up to four LOD index ranges over shared vertices,
each with an object-space geometric error. Both submission paths pick the
coarsest level whose error, scaled by the object's largest axis scale and
projected from the nearest point of its bounding sphere at internal render
resolution, is at most `RenderSettings::lodErrorThresholdPixels` (zero keeps
full detail). The CPU path selects per render item, and shadows reuse the
camera's choice. The GPU path gives each level its own batch, chains a
primitive's levels through the batch records, and selects in the cull shader
with the same formula; every object reserves an instance slot in each of its
level batches.

With GPU-driven submission the directional shadow cascades are culled on the
GPU as well. `GpuDrivenPass` owns one output region per view
(`GpuCullLayout.hpp`): the camera's early and late phases, then one
frustum-only region per cascade. A frustum-only cull tests the sphere against
the cascade's orthographic clip volume with the same predicate the CPU cascade
cull uses, never reads the depth pyramid or the camera's visibility history,
and selects LODs from the camera's view, so cascades match the CPU path's
choice. Only cascades the cache marks dirty are culled and redrawn; their draws
use the shadow pipeline with the GPU-driven camera set, whose object binding is
the cull output, so `shadow.vert` is shared by both paths. Blended materials
never enter the GPU-driven records, matching the CPU cascade cull. The CPU path
remains the fallback whenever GPU-driven submission is off, and
`ZENITH_DISABLE_GPU_SHADOW_CULLING` selects it explicitly for comparison.

### Depth prepass and screen-space inputs

`RenderSettings::depthPrepass` (on by default) draws opaque and masked
surfaces into scene depth and a view-space normal target before shading. With
GPU-driven submission the two-phase cull runs inside the prepass, and the
shading pass redraws both phases' compacted draws without culling again. The
prepass vertex shader repeats `mesh.vert`'s position math with `precise` and
`invariant gl_Position`, so opaque shading tests `EQUAL` with depth writes off
and every opaque pixel shades once; masked materials alpha-test in both passes.
After the prepass the depth pyramid is rebuilt from the complete opaque depth
on both submission paths; with GPU-driven submission the GPU skips that rebuild
when the late phase emitted nothing, since the occlusion build already holds
the complete depth. With the prepass off, the main pass tests and writes
depth as before and the outputs below are unavailable.

Contract for screen-space consumers (`MeshPass::sceneGeometry` returns
`SceneGeometryViews`, valid for the frame being recorded only when the prepass
ran):

- Normals: `PostProcessPass::normalFormat` (`A2B10G10R10_UNORM`), in
  `SHADER_READ_ONLY_OPTIMAL`, output-sized with the render extent at the
  top-left. `rg` is the octahedral view-space normal mapped to `[0, 1]` (view
  space is right-handed, +Z towards the camera; back faces of double-sided
  surfaces are flipped towards the viewer), `b` is perceptual roughness clamped
  like the shading pass, and `a` is 1 where an opaque surface was written (0 on
  clear). `SceneNormalEncoding.hpp` mirrors the encoding on the CPU.
- Specular: `PostProcessPass::specularFormat` (`R8G8B8A8_SRGB`), in
  `SHADER_READ_ONLY_OPTIMAL`, same extents as the normals, written only on
  frames that trace screen-space reflections (otherwise the attachment is
  `VK_NULL_HANDLE` and `SceneGeometryViews::specular` is null). `rgb` is the
  linear specular reflectance at normal incidence (F0 =
  `mix(0.04, baseColor, metallic)`, stored through the sRGB transfer) and `a`
  is coverage. Allocated per frame slot on first use, 4 bytes per output pixel;
  `SceneSpecularEncoding.hpp` mirrors the encoding on the CPU.
- Depth pyramid: `DepthPyramid`, one RG32F image in `GENERAL` layout,
  sampled with its nearest/clamp sampler via `texelFetch`. `r` is the farthest
  (minimum, reversed Z) and `g` the closest (maximum) depth a texel covers.
  Level 0 copies scene depth over the render extent; level `L` spans
  `max(renderExtent >> L, 1)` texels in the top-left of its storage mip, and
  base pixel `p` lies in texel `min(p >> L, levelExtent - 1)`. Normalized UVs
  must be scaled by `levelExtent / storageExtent` of the sampled level.
  Readers in compute or fragment shaders are ordered before the next rebuild.

### Material overrides

A `MaterialOverrideComponent` on a mesh renderer changes how that entity
draws without touching the shared material. When its runtime-only `material`
handle is live it replaces every primitive's material; its factors then
multiply the base color, add emission, and multiply roughness and metallic
(`applyMaterialOverride` in `MaterialRecord.*`, CPU-tested). A non-identity
override draws with a material record derived for that pair of material and
factors (bit-exact), shared by every item using the same pair, so a thousand
crates with one "danger red" tint cost one record. Derived records share the
4096 material record slots, are rewritten every frame from the current
material (so material edits show through), and are freed when a packet build
no longer uses them. Overrides change only the per-object material index both
the CPU-submitted and GPU-driven paths read, so batching and pipelines (which
follow the material's alpha mode and sidedness) are unchanged. Packets are
rebuilt when the scene's material revision changes: after writing the
component, call `Scene::markMaterialDirty` (editor edits do).

`MeshPass` is one class whose member definitions are split by responsibility:

| File | Contents |
| --- | --- |
| `MeshPass.cpp` | `init`, `shutdown`, deferred uploads, unused-asset pruning, `uiTexture` |
| `MeshPassViews.cpp` | per-view create/resize/destroy and camera sets |
| `MeshPassDescriptors.cpp` | descriptor layouts, pools, and per-view descriptor writes; decal preparation |
| `MeshPassPipelines.cpp` | graphics pipelines and shading-variant pipeline sets |
| `MeshPassResources.cpp` | geometry pages, `ensureMesh`/`ensureTexture`/`ensureMaterial`, texture slots, material records, bindless updates |
| `MeshPassTextureStreaming.cpp` | texture uploads and mip streaming |
| `MeshPassRenderItems.cpp` | render-item building, incremental updates, and LOD selection |
| `MeshPassShadows.cpp` | directional and local shadow maps, per-view shadow state |
| `MeshPassRecord.cpp` | `FrameState` use, `prepareViewFrame`, `record`, `recordFrameStage` |
| `MeshPassRecordView.cpp` | `recordView` |
| `MeshPassSceneDraws.cpp` | render-pass begin helpers, depth prepass, CPU-submitted and shaded item draws |
| `MeshPassGpuDriven.cpp` | GPU-driven frame preparation and indirect group draws |
| `MeshPassEffects.cpp` | SSR, transmission background, volumetric fog, particles, ambient occlusion |
| `MeshPassEditorOverlays.cpp` | picks, selection mask, outlines, wireframe overlay |
| `MeshPassInternal.hpp` | private constants, GPU record layouts, helpers shared by those files, and `MeshPass::FrameState` |

## `src/engine/graphics/MaterialRecord.*`

The GPU form of `MaterialAsset` in the camera set's material buffer (binding 4):
factors, bindless texture slots, and the lobe bit mask that gates the KHR
material extension lobes in `mesh.frag`. `packMaterialRecord` is CPU-testable;
the shaders mirror the 192-byte std430 layout.

## `src/engine/graphics/passes/TransmissionBackground.*`

A mip-chained copy of the opaque scene (with sky and screen-space reflections)
that transmissive materials sample through camera binding 24. `MeshPass`
captures it between opaque and blended surfaces, ending and resuming the scene
pass, only on frames that draw a transmissive surface or water; the copy
stretches the render extent over the whole image so every mip is valid.

## Water

A material with `MaterialWater` enabled packs the `material_lobes::water` bit
and its wave settings in `MaterialRecord::water` (its absorption coefficient
and fallback depth travel in `volume`). Water is drawn with the blended
surfaces and shaded in `mesh.frag` (`waterNormal`, `waterTransmitted`):

- Six directional waves, each moving at the deep-water speed of its length,
  tilt the normal of upward faces; waves shorter than a few pixels fade out so
  distant water stays calm. The waves run on `CameraData::surfaceAnimation.x`,
  which `MeshPass::advanceSurfaceAnimation` advances once per frame by
  wall-clock time (or by the fixed frame step, so captures stay deterministic).
- The opaque depth behind the surface gives the path through the water: camera
  binding 25 is the view's depth pyramid, whose level 0 is the opaque depth
  after the prepass. `surfaceAnimation.y` says whether this frame has one;
  without a prepass the material's fallback depth applies.
- The transmission background shows the bed, shifted by the waves (never onto
  something in front of the water), absorbed by Beer-Lambert over the path,
  and filled with the base color lit by the sky and sun as it deepens. The
  edge fades in over `shore_fade` metres of depth, and the surface reflects
  the sky and sun through the usual specular terms.
- Water reflects the opaque scene around it (`waterScreenReflection`): the
  reflected ray marches in world space with growing steps, projecting each
  onto the scene depth, and a hit takes the transmission background's color,
  fading toward the screen edges and the end of the march, where the
  environment's reflection remains. The engine's own screen-space reflections
  trace opaque surfaces only, so they never reach water. Water casts no
  shadow.

## Terrain

A material with `MaterialTerrain` enabled packs the `material_lobes::terrain`
bit and four layers (albedo, normal, ARM slots and a tile size in metres) in
`MaterialRecord::terrainAlbedo`/`terrainNormal`/`terrainArm`/`terrainTileSize`.
`mesh.frag` (`terrainSurface`) replaces the base textures with a blend of the
layers:

- The mesh's vertex weights (`Vertex::weights`, passed as `surfaceWeights`)
  give each layer's share; they are normalized in the shader. Terrain meshes
  are meant to be unskinned, since skinned meshes use the weights for joints.
- Layers 0-2 are projected from above (world xz over the tile size), so
  neighbouring meshes line up with no texture coordinates; layer 3 (rock) is
  projected from all three axes and blended by the normal, so cliffs do not
  stretch.
- Each sampled layer's brightness raises its weight, and only layers within a
  narrow band of the strongest survive, so transitions follow the texture
  detail (sand between stones, grass in cracks) instead of a smooth fade.

## Ground-truth ambient occlusion

`RenderSettings::ambientOcclusion` selects GTAO's resolution and sampling
pattern (`ambient_occlusion::qualityProfile`); both qualities write a
render-resolution R8 image, so shading is identical:

| Quality | Traced texels | Slices x steps per side | Horizon depth | Denoise |
| --- | --- | --- | --- | --- |
| **HalfResolution** (recommended) | one per 2x2 pixels | 2 x 4 | pyramid level 1, farthest/closest depth in a checkerboard | `gtao_upsample.comp`: 4x4 raw texels per render pixel, depth-weighted against the pixel's own view depth |
| **FullResolution** | every pixel | 3 x 4 | pyramid level 0 | `gtao_denoise.comp`: depth-aware 5x5 |

Both rotate slices with a 4x4 ordered dither (advanced per frame under TAA),
and the denoise window holds each of its 16 phases, so half resolution still
averages 32 slice directions per pixel. The clamp to [0, 1] and the occlusion
power apply after the average: clamping each few-slice estimate first darkened
open surfaces far from the view axis. The scope is `gpu.ambient_occlusion` and
the budget `RenderPassBudgets::ambientOcclusion` (zero, unenforced, like every
default budget; half resolution fits 0.2 ms at 720p and 0.35 ms at 1080p on the
reference GPU).

## Single-pass depth pyramid

`DepthPyramid` builds every level in one dispatch
(`shaders/depth_pyramid_single_pass.comp`, structured after AMD FidelityFX SPD):
each 256-thread workgroup reduces a 64x64 tile through six levels in shared
memory, and a global atomic counter elects the last workgroup, which completes
the edge texels of levels whose extent does not tile the base exactly and then
reduces the remaining levels. The result is identical to the per-level build
(`engine.DepthPrepass` checks both against a brute-force min/max on odd
sizes). Devices without `shaderStorageImageArrayDynamicIndexing` fall back to
the per-level build and log it once; `ZENITH_DEPTH_PYRAMID_PER_LEVEL` forces it.

With GPU-driven submission and the depth prepass, the pyramid used to be built
twice per frame: once from the early phase's depth for the late cull, and again
after the late phase for screen-space effects. The second build now passes the
late phase's emitted-instance count (`GpuDrivenPass::lateEmittedCount`) and the
shader exits when it is zero, since the depth is then unchanged. In the steady
state the pyramid is built once per frame; when objects become visible the
rebuild still runs, so SSR and GTAO always see the complete depth. Using last
frame's pyramid for the late cull instead would cull objects disoccluded by
camera motion for a frame. `gpu.depth_pyramid` measures only the post-prepass
build on that path (the occlusion build counts toward `gpu.depth_prepass`).

## GPU-culled shadow cascades

GPU-driven submission culls each dirty directional shadow cascade on the GPU
(`GpuDrivenPass::cullFrustumView`) and draws it with indirect commands. There is
no separate render setting: the technique's Off fallback is the CPU cascade
cull, used whenever GPU-driven submission is off or unsupported, and
`ZENITH_DISABLE_GPU_SHADOW_CULLING` forces it for A/B measurement. Culling has its
own timestamp scope (`gpu.shadow_culling`) and budget
(`RenderPassBudgets::shadowCulling`, serialized as `shadowCulling`). The three
cascade regions add about 33 MB of device memory to the GPU-driven buffers
(visible instances, counters, and commands sized like a camera phase). With GPU
culling, `RenderStats::shadowCasterCount` reports the casters a retired frame
emitted, like `visibleCount`, and shadow triangles are not counted.

## GPU particles

`ParticleEmitterComponent` (scene document `particle_emitter`, every field
optional) authors an emitter; `RenderSettings::particles` (`particles` in render
settings: 0 Off, 1 Low with 16,384 particles, 2 High with 131,072) sets the
budget every emitter shares, and the editor's Graphics panel exposes it next to
the volumetric fog. Emitters take their slots in scene-id order, so with too
small a budget the last emitters get fewer slots or none; within its ring an
emitter replaces its oldest particle once full. At most 64 emitters and eight
sprite textures (one is the procedural round sprite) are used per frame.

Captures are reproducible: with a fixed frame time (implied by
`ZENITH_CAPTURE_FRAME`) spawn counts, pool slots, and random values depend only
on the frame number and the emitters' scene ids, and the back-to-front sort
breaks depth ties by slot. Two consecutive captures of a 2,400-particle ember
and smoke scene differed by at most 2 8-bit steps (RMSE 0.13), the same noise
as captures without particles.

## Reflection probe blending

`RenderSettings::reflectionProbeBlending` selects how reflection probes reach
shading when image-based lighting and `reflectionProbes` are on:

- **PerPixel** (default): up to eight probes nearest the camera stay resident.
  `EnvironmentPass` copies each one's prefiltered specular cube into a slot of
  one cube-map array (binding 16) once, when it becomes resident, building at
  most two new probe environments per frame. `ReflectionProbeClusterPass`
  uploads the probe records and their irradiance and
  `reflection_probe_clusters.comp` assigns each probe's influence box (its box
  expanded by the blend distance) to the light-cluster grid (binding 17).
  `mesh.frag` blends up to four of its cluster's probes per pixel with
  box-projected specular lookups and blended irradiance, matching
  `blendReflectionProbes`; the scene environment fills the remainder across
  each probe's blend distance. The GPU scope is `gpu.reflection_probe_clusters`
  with the `reflectionProbeClusters` budget; `reflection_probes` counts the
  resident probes.
- **CameraOnly** (the Off fallback): the dominant probe at the camera is
  blended over the whole frame, as before. Validation selects it, with a
  fallback message, when the device lacks per-pixel support.

`engine.ReflectionProbe` covers box projection against an independent slab
intersection, weight normalization against `blendReflectionProbes`, resident
selection, and conservative cluster assignment against brute-force frustum
samples. The showcase project's west and east halves carry two overlapping
probes with distinct environments (`tools/showcase/build_probe_environments.py`).

## Clustered decals

`DecalComponent` is an oriented box projector: the entity's world transform
applied to `[-halfExtents, halfExtents]`, projecting along its local -Y (an
unrotated decal lands on floors; pitch it 90 degrees for a wall), with u along
local +X and v along local +Z. It carries an optional albedo texture (sRGB; its
alpha masks every effect), an optional tangent-space normal texture (+Y up),
a linear tint and opacity, per-property strengths for base color, normal
(without a normal texture it flattens the surface toward its geometric normal,
as paint or standing water fills bumps), roughness and metallic overrides, an angle fade (full coverage up to
`angleFadeStartRadians` between the surface's geometric normal and the
projector axis, none past `angleFadeEndRadians`, so walls next to a floor decal
and back faces stay clean), and a sort order. Decals also fade across the outer
quarter of the box depth. Scene documents store it as the optional `decal`
component; only `half_extents` is required.

`RenderSettings::decals` (`DecalQuality`, serialized as `decals` by value) is
On by default; Off is the fallback and records no work. Each frame
`MeshPass::prepareDecals` gathers decals in application order (ascending sort
order, then stable scene id), keeps the 1024 nearest the camera, resolves their textures
to bindless slots, and packs them with conservative view-space bounds.
`DecalClusterPass` uploads the records (camera binding 19) and
`decal_clusters.comp` lists up to 31 decals per light-cluster froxel in
application order (binding 20). `applyDecals` in `mesh.frag` walks the
fragment's cluster list before lighting and blends each decal into base color,
the (material-mapped) normal, roughness and metallic of opaque and masked
surfaces; textures filter with explicit gradients from the world-position
derivatives. Decals do not change the depth prepass normals, so GTAO and
screen-space reflections see the undecorated surface.

The GPU scope is `gpu.decal_clusters` (timestamp slots 51/52) with the
`decalClusters` budget; the benchmark also reports `decals.count`,
`decals.dropped`, `decals.discarded_assignments`, `decals.maximum_per_cluster`
and `render.decal_bytes` (records and cluster lists, about 0.6 MB per frame in
flight). `engine.Decal` covers decal-space projection, the angle
and depth fades, ordering and nearest selection, conservative cluster
assignment with overflow counting against a brute-force test, GPU packing,
scene-document round trips with the shipped textures, and the setting's
serialization and fallback. `tools/showcase/build_decal_textures.py` writes the
showcase textures under `assets/showcase/decals/`.

## KHR material extensions

`ModelImporter` reads `KHR_materials_clearcoat`, `KHR_materials_sheen`,
`KHR_materials_specular`, `KHR_materials_transmission`, `KHR_materials_volume`
(thickness and attenuation factors; no thickness texture) and
`KHR_materials_ior` into `MaterialAsset`. Absent extensions keep neutral
defaults (ior 1.5, specular 1 and white, no coat, black sheen, no
transmission), which shade exactly like plain metallic-roughness. Texture
slots are classified for cooking like the core ones: coat normals are normal
maps, sheen and specular colors are sRGB color, the rest linear data.

`MaterialRecord` (`engine/graphics/MaterialRecord.hpp`, 192 bytes, mirrored in
`mesh.frag`, `shadow.frag` and `depth_normal.frag`) carries a lobe bit mask;
`mesh.frag` evaluates and samples a lobe only when its bit is set, so a
material without extensions pays a few uniform branches. Shading follows the
glTF specifications and the Khronos sample viewer:

- **ior / specular**: dielectric F0 = min(((ior - 1) / (ior + 1))^2 *
  specular color, 1); the specular factor scales the dielectric Fresnel term,
  grazing value included, in direct, image-based and hemisphere lighting. The
  depth prepass writes the same F0 for screen-space reflections.
- **sheen**: Charlie distribution with the Estevez-Kulla visibility per light;
  the layers below are scaled by 1 - max(sheen color) * albedo, where the
  directional albedo lives in the environment BRDF table's blue channel
  (`env_brdf_lut.comp`, CPU reference `integrateSheenAlbedo`). Ambient sheen is
  lit by the cosine-weighted environment (irradiance), not a Charlie-filtered
  cube.
- **clearcoat**: a second GGX lobe (ior 1.5) on the coat normal (the coat
  normal map, or the geometric normal) for every light and for prefiltered
  image-based reflections; everything below, emission included, is attenuated
  by 1 - coat * F(n.v).
- **transmission**: transmissive materials are translucent
  (`MaterialAsset::translucent()`): they draw with blended surfaces, cast no
  shadows and stay out of the depth prepass, ambient occlusion and the
  irradiance baker. On frames that draw one, `TransmissionBackground` ends the
  scene pass after opaque surfaces, the sky and screen-space reflections,
  blits the HDR target into a mip-chained copy, and resumes. The surface
  replaces `transmission` of its diffuse base with that copy, tinted by the
  base color and reduced by its split-sum reflectance, blurred by
  roughness * clamp(2 ior - 2, 0, 1) through the mips. Without a volume the
  surface is thin-walled (straight through); a `KHR_materials_volume`
  thickness refracts the view ray by the ior and absorbs along it with the
  Beer-Lambert attenuation. Limitations: thickness is in world units (object
  scale is ignored), lights behind the surface reach it only through the copy,
  and transmissive surfaces do not see each other (the copy holds only the
  opaque scene). An opaque-mode transmissive surface writes alpha 1; with
  `BLEND` its alpha still composites over the result.
- The fast lighting variant (`PHYSICALLY_BASED_LIGHTING` off) keeps only
  transmission, without the reflectance term.

`gpu.transmission_background` times the copy (inside the `gpu.blended` span and
subtracted from it); the lobes are part of `gpu.opaque` and `gpu.blended`
shading. `engine.MaterialExtension` covers import, defaults, texture usages,
record packing and the sheen albedo; `tools/showcase/build_materials.py`
generates the material gallery (`assets/showcase/materials/`).

The transmission copy holds one RGBA16F image of the render target's size with
a full mip chain (about 22 MB at 1920x1080), paid only on frames that draw a
transmissive surface.

## Local-light shadows

`RenderSettings::localShadows` (`LocalShadowQuality`) shadows point and spot
lights whose components set `castsShadows`. Off is the fallback: lights stay
unshadowed, a 64x64 placeholder atlas stays bound, and no tile pass runs.
Validation also turns it Off when clustered local lighting is disabled or
unsupported. Low, Medium, and High pick the atlas size (2048 or 4096 texels),
the default tile sizes, the number of shadowed lights (8, 16, 24), and the
per-frame tile budget (6, 12, 24 tiles); see `localShadowProfile`.

Tiles keep their depth across frames and are redrawn only when their light
moves, a caster within the light's range moves or animates, or the caster set
or materials change; dirty tiles beyond the budget keep shading with their
previous depth and projection. The benchmark reports `gpu.local_shadows` and
`cpu.local_shadows` against `RenderPassBudgets::localShadows`, plus
`local_shadows.tiles_rendered`, `tiles_cached`, `tiles_pending`,
`lights_rejected` (no atlas space), and `render.local_shadow_atlas_bytes`.
`engine.LocalShadow` covers request order, cache invalidation, budget
selection, and the face projections on the CPU.

## View shading modes

`RenderSettings::viewShading` selects Rendered, Material Preview, Solid or
Wireframe, after Blender's viewport shading. The frame settings carry a
project-wide value (the editor's Graphics Settings panel has a "Viewport
shading" combo, serialized by value), and each editor viewport sets its own
through a per-view settings override (`RenderRuntime::setViewSettings`)
holding the already reduced `viewShadingFrameSettings`. Documents written before
the field existed load as Rendered. Every mode is one
recording path with a reduced feature set:

- `viewShadingFrameSettings` (`ViewShading.hpp`) turns the requested settings
  into the frame's. Rendered returns them unchanged, so its output is
  untouched. The other modes drop shadows, local lights and their shadows,
  reflection probes, the irradiance volume, image-based lighting and the
  environment background, ambient occlusion, screen-space reflections,
  volumetric fog, bloom, motion blur, depth of field and colour grading, and
  fix exposure at 0 EV manual. Material Preview keeps decals, particles and
  temporal anti-aliasing; Solid and Wireframe drop decals and particles, and
  Wireframe swaps TAA (and TAA upsampling) for FXAA because jitter smears
  one-pixel edges, and turns off the depth prepass and GPU-driven submission
  (no surface depth is drawn and nothing is occlusion culled, so every edge
  shows). The tone-mapping operator, anti-aliasing, the upscaler, resolution
  and LOD stay as requested, so the editor grid, selection mask and outlines
  work unchanged.
- `Renderer::recordFrame` substitutes those settings for the frame through
  `ScopedViewShadingSettings`, so the frame plan, the render-graph shape and
  every pass agree, while the editor and serialization keep seeing the request.
- `MeshPass` replaces the scene's directional and hemisphere light in the
  camera constants with a camera-relative studio rig (`studioLighting`: a key
  light above, behind and left of the camera plus a neutral sky and ground),
  so Material Preview shades textured materials through the ordinary
  hemisphere path.
- Solid and Wireframe select the `SOLID_SURFACES` specialization of
  `mesh.frag`: one 0.8 clay albedo, geometric normals, the studio key and
  hemisphere and a faint rim, with no texture reads except base-colour alpha
  for cutouts and blending.
- Wireframe also builds polygon-mode LINE overlay pipelines
  (`mesh_wireframe.frag`) and, like Blender's default wireframe shading, draws
  only the edges of every surface, through each other (x-ray): no surface is
  shaded (`drawsSurfaces`) or written to depth, and the CPU-submitted items
  draw on the cleared scene in one light colour. The edges pass every depth
  test, write no depth and write only scene RGB, so the selection mask and
  outlines still work. Line width is 1 (`wideLines` is not used).

Wireframe needs `fillModeNonSolid`. `RenderCapabilities::wireframe` reports
it; `validateRenderSettings` falls back to Solid with a message, and the frame
settings resolve the same way for settings forced past validation.
`ZENITH_DISABLE_WIREFRAME=1` exercises that fallback on capable devices.
`ZENITH_FORCE_VIEW_SHADING` selects a mode for smoke runs; the validation
matrix runs each mode on both submission paths with every feature requested.
