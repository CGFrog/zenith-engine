# Graphics scene passes

Part of the [repository guide](../REPOSITORY_GUIDE.md).

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
surface. The contract is in
`docs/RENDERER_VALIDATION.md` (View shading modes);
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
  material and factor pairs draw their plain material (logged once per
  episode).

Local lights and local shadow tiles already keep the nearest within their
limits. The GPU-driven limits still fail the frame and log an error each frame
they are exceeded: the draw-group limit (64 pipeline bucket and geometry page
pairs, a geometry-arena layout limit, not a per-scene count) and the LOD
instance capacity (`maximumGpuDrivenInstances`).

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
pass, only on frames that draw a transmissive surface; the copy stretches the
render extent over the whole image so every mip is valid.
