# Shaders

Part of the [repository guide](../REPOSITORY_GUIDE.md).

| File | Stage and job |
| --- | --- |
| `shaders/mesh.vert` | Transforms mesh vertices and supplies world-space attributes. |
| `shaders/mesh.frag` | Samples glTF materials, evaluates GGX lighting, shadows, occlusion, and emission; its `SOLID_SURFACES` variant draws Solid view shading's clay. Its shading is split by concern into `shaders/mesh/*.glsl` (see [mesh.frag parts](#meshfrag-parts)). |
| `shaders/mesh_wireframe.frag` | Colours the polygon-mode LINE edges of Wireframe view shading. |
| `shaders/shadow.vert` | Transforms geometry into directional-light depth space. |
| `shaders/local_shadow.vert` | Transforms shadow casters into one local-light shadow atlas tile. |
| `shaders/depth_normal.vert` / `.frag` | Depth prepass: invariant positions shared with `mesh.vert`, alpha masking, and encoded view-space normals plus roughness. |
| `shaders/depth_pyramid.comp` | Reduces scene depth into the min/max depth pyramid one level per dispatch. |
| `shaders/fullscreen.vert` | Generates a fullscreen triangle without a vertex buffer. |
| `shaders/post.frag` | Combines HDR color and the editor grid, applies FXAA, exposure, bloom and tone mapping. |
| `shaders/color_grade.frag` | Applies the 3D LUT grade, vignette and film grain to the tone-mapped frame. |
| `shaders/luminance_histogram.comp` | Bins the valid HDR region into a 256-bin log2-luminance histogram. |
| `shaders/exposure_adapt.comp` | Percentile-averages the histogram and adapts the persistent exposure. |
| `shaders/bloom_downsample.comp` | 13-tap bloom downsample; Karis average on the first level. |
| `shaders/bloom_upsample.comp` | 3x3 tent bloom upsample accumulated into the next larger level. |
| `shaders/gtao.comp` | Integrates ground-truth ambient occlusion from prepass depth and normals. |
| `shaders/gtao_denoise.comp` | Depth-aware 5x5 blur of the raw GTAO visibility (full resolution). |
| `shaders/gtao_upsample.comp` | Depth-aware denoise and upsample of half-resolution GTAO to the render resolution. |
| `shaders/ssr_trace.comp` | Hi-Z screen-space reflection trace over the closest-depth pyramid. |
| `shaders/ssr_composite.comp` | Blends traced reflections into the HDR scene before blended surfaces. |
| `shaders/light_clusters.comp` | Assigns local lights to view clusters for clustered forward shading. |
| `shaders/decal_clusters.comp` | Lists each view cluster's decals, in application order, for `mesh.frag`. |
| `shaders/volumetric_fog_inject.comp` | Evaluates and lights the fog medium per froxel and reprojects history. |
| `shaders/volumetric_fog_integrate.comp` | Integrates froxel fog front to back into scattering and transmittance. |
| `shaders/particle_emit.comp` | Writes each frame's new particles into their emitters' rings. |
| `shaders/particle_simulate.comp` | Moves, expires, culls, and lights particles and appends the visible ones to the sort and draw. |
| `shaders/particle_sort.comp` | One bitonic sorting-network dispatch over the particle sort keys. |
| `shaders/particle.vert`, `shaders/particle.frag` | Camera-facing particle quads: premultiplied, soft, fogged, with the reactive mask. |
| `shaders/depth_pyramid.comp` | Reduces scene depth into a conservative min-depth (reversed-Z) pyramid. |
| `shaders/gpu_visibility.comp` | Runs early/late two-phase frustum and occlusion culling. |
| `shaders/compact_draws.comp` | Packs non-empty batches into indirect-count draw commands. |
| `shaders/taa_resolve.comp` | Temporal anti-aliasing and upsampling into output-resolution history. |
| `shaders/dof_prepare.comp` | Depth of field: half-resolution scene downsample with the signed circle of confusion. |
| `shaders/dof_gather.comp` | Depth of field: half-resolution golden-angle bokeh gather. |
| `shaders/dof_composite.comp` | Depth of field: blends the upsampled blur over the sharp scene. |

| `shaders/motion_blur_tile_max.comp` | Reduces per-pixel motion blur velocity to each tile's largest. |
| `shaders/motion_blur_neighbor_max.comp` | Largest tile velocity in each 3x3 tile neighbourhood. |
| `shaders/motion_blur_reconstruct.comp` | Depth-aware motion blur gather along the neighbourhood velocity. |
| `shaders/fsr_easu.comp` | FSR 1 edge-adaptive upsampling of the tone-mapped render-resolution frame. |
| `shaders/fsr_rcas.frag` | FSR 1 contrast-adaptive sharpening into the output. |
| `shaders/env_equirect_to_cube.comp` | Resamples an equirectangular radiance map into an environment cube. |
| `shaders/env_procedural_sky.comp` | Writes the analytic sky cube used when no environment map is loaded. |
| `shaders/env_prefilter_specular.comp` | GGX-prefilters one specular cube mip with mip-filtered importance sampling. |
| `shaders/env_irradiance_sh.comp` | Projects an environment cube onto order-2 irradiance spherical harmonics. |
| `shaders/env_brdf_lut.comp` | Integrates the split-sum environment BRDF table. |
| `shaders/skybox.vert` / `skybox.frag` | Draws the environment at the far plane with camera-only motion vectors. |

Recognized shader extensions automatically produce matching
`build/shaders/<relative-path>.spv` files.

## Shared includes

GLSL that more than one shader needs lives once in `shaders/include/*.glsl`
and is pulled in with `#include "<name>.glsl"` (the shader enables
`GL_GOOGLE_include_directive`). The `.glsl` extension is not a stage, so an
include is never compiled alone. glslang writes a depfile per shader, so
editing an include rebuilds exactly the shaders that use it.

Rules:

- Every GPU record mirrored from C++ lives in one include, and the C++ type's
  comment names it. A C++ `static_assert` pins the record's size.
- Includes hold types, constants and functions. A shader declares its own
  bindings, because the binding numbers differ between passes. The exception
  is a binding that is part of one descriptor set layout every includer
  shares: the camera block (`camera.glsl`), the bindless material set
  (`material_textures.glsl`), the particle frame block
  (`particle_simulation.glsl`) and push constants shared by one feature's
  passes.
- A function that reads a resource the includer declares says so in the
  include's header comment (`irradiance_probe_depth.glsl` reads
  `irradianceVolume`). Prefer passing the resource as a parameter when GLSL
  allows it (samplers, as in `depth_of_field.glsl`).
- Each include has an include guard (`ZENITH_<NAME>_GLSL`) and opens with a
  comment saying what it is for.

| Include | Contents | C++ mirror |
| --- | --- | --- |
| `constants.glsl` | `PI` | – |
| `camera.glsl` | The camera uniform block (set 0, binding 0) | `mesh_pass_detail::CameraData` |
| `render_object.glsl` | `RenderObject` | `mesh_pass_detail::ObjectData` |
| `material.glsl` | `MaterialRecord`, `LOBE_*` | `MaterialRecord`, `material_lobes` |
| `material_textures.glsl` | Bindless material set, `sampleMaterialTexture` | – |
| `dielectric_specular.glsl` | `dielectricSpecular` (KHR_materials_ior/specular) | – |
| `local_light.glsl` | `LocalLight` | `GpuLocalLight` |
| `local_shadow_tile.glsl` | `LocalShadowTile` | `GpuLocalShadowTile` |
| `irradiance_volume.glsl` | `IrradianceProbe` | `irradiance_volume::GpuProbe` |
| `irradiance_probe_depth.glsl` | Probe depth moments and Chebyshev visibility | `irradiance_volume` functions |
| `octahedral.glsl` | Octahedral encode and decode | `irradiance_volume`, `vertex_packing` |
| `vertex_packing.glsl` | `decodeTangent` | `vertex_packing::unpackTangent` |
| `reflection_probe.glsl` | `ReflectionProbeRecord`, probe limits | `GpuReflectionProbe` |
| `decal.glsl` | `DecalRecord`, decal limits and flags | `GpuDecal`, `Decals.hpp` limits |
| `sheen.glsl` | Charlie sheen distribution and visibility | `EnvironmentLighting.cpp` |
| `environment_cube.glsl` | `cubeFaceDirection` | – |
| `importance_sampling.glsl` | `radicalInverse`, `importanceSampleGgx` | – |
| `hash.glsl` | `pcgHash` | – |
| `particle.glsl` | `Particle`, `FLAG_*` | `GpuParticle`, `particles::flag*` |
| `particle_simulation.glsl` | `ParticleFrame` block, `Emitter` | `GpuParticleFrame`, `GpuParticleEmitter` |
| `gpu_cull.glsl` | `BatchRecord` | `GpuDrivenBatch` |
| `depth_of_field.glsl` | DoF push constants, `circleOfConfusion` | `DepthOfFieldSettingsBlock` |
| `motion_blur.glsl` | Motion blur push constants, `velocityAndDistance` | `MotionBlurSettings` |
| `debug_line.glsl` | Debug line push constants | `DebugLinePushConstants` |
| `id_draw.glsl` | Object-ID push constants | `IdBufferDraw` |

## mesh.frag parts

`mesh.frag` declares the scene pass's bindings, stage inputs and
specialization constants, the screen-space helpers that read per-frame
buffers (cluster index, ambient occlusion, fog, motion), and `main()`. The
shading it calls lives in `shaders/mesh/`, one concern per file, included in
dependency order. The parts are not shared includes: each reads `mesh.frag`'s
declarations and the parts before it, and is compiled only as part of
`mesh.frag`.

| Part | Contents |
| --- | --- |
| `terrain.glsl` | Terrain layer blending (`LOBE_TERRAIN`) |
| `brdf.glsl` | GGX distribution, Smith geometry, Schlick Fresnel, `directResponse` |
| `shadows.glsl` | Cascaded sun shadows and local-light atlas shadows |
| `local_light_radiance.glsl` | One clustered local light's radiance at the fragment |
| `irradiance.glsl` | SH irradiance and the irradiance volume |
| `reflection_probes.glsl` | Per-pixel probe blending and probe-weighted environment lookups |
| `ambient_lighting.glsl` | Specular occlusion, image-based lighting and the hemisphere fallback |
| `decals.glsl` | Clustered decals applied before lighting |
| `surface_lobes.glsl` | KHR specular, sheen, clearcoat and transmission lobes |
| `water.glsl` | Water waves, transmission and screen-space reflections (`LOBE_WATER`) |
