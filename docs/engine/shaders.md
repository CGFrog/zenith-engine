# Shaders

Part of the [repository guide](../REPOSITORY_GUIDE.md).

| File | Stage and job |
| --- | --- |
| `shaders/mesh.vert` | Transforms mesh vertices and supplies world-space attributes. |
| `shaders/mesh.frag` | Samples glTF materials, evaluates GGX lighting, shadows, occlusion, and emission; its `SOLID_SURFACES` variant draws Solid view shading's clay. |
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
