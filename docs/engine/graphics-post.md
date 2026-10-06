# Graphics post-processing

Part of the [repository guide](../REPOSITORY_GUIDE.md).

The file sections come first. The feature contracts (settings, fallbacks, GPU
scopes, budgets, and tests) follow them: [Upscaling](#upscaling),
[Motion blur](#motion-blur), [Depth of field](#depth-of-field),
[Colour grading](#colour-grading), and
[Post-process descriptors and FXAA](#post-process-descriptors-and-fxaa).

## `src/engine/graphics/passes/PostProcessPass.*`

Owns one RGBA16F HDR scene target, motion/reactive target, prepass normal
target (A2B10G10R10) per frame slot, plus the view's `ScreenSpaceReflectionPass`,
`TemporalResolvePass`, `DepthOfFieldPass`, `MotionBlurPass`, and
`BloomExposurePass`. `recordReflections()` runs inside the scene pass, between
opaque and blended surfaces, and blends screen-space reflections into the HDR
target when enabled and the depth prepass ran. With TAA selected the resolve
runs first and later effects read its output-resolution result; otherwise they
read the HDR target. Depth of field (`focusScene()`) and then motion blur
(`recordMotionBlur()`) each replace that scene with their output when enabled,
and `bindSceneColor()` binds the final one for the fullscreen pass (as the
resolved scene with TAA, else as the HDR scene that FXAA samples). The final
fullscreen pass composites the editor grid over the resolved scene (TAA) or the
HDR target, applies FXAA to that composite when selected, then exposure, the
bloom mix, and tone mapping (AgX by default, AgX punchy or legacy ACES). It
writes the output, or the `ColorGradePass` display target when a grade effect
is enabled; with FSR 1 it writes the render-resolution region of
`SpatialUpscalePass`'s input, whose RCAS output takes its place. Its
descriptor set is 0 HDR scene, 1 depth, 2 resolved scene,
3 bloom mip 0, 4 exposure buffer. Half-resolution effect tiers render into
bounded sub-extents.

With `RenderSettings::upscaler` set to FSR 1 the composite runs at render
resolution into `SpatialUpscalePass`'s input (TAA then resolves at render
resolution and reaches the composite through binding 0), and EASU and RCAS
bring it to the output (or to the colour grade's display target when a grade
effect is enabled).

## `src/engine/graphics/passes/ColorGradePass.*`

Display-referred finishing stage (`shaders/color_grade.frag`) after tone
mapping: a 3D LUT grade applied to sRGB-encoded values with a blend strength,
a vignette, and per-frame film grain seeded by the frame serial. Each effect is
selected independently in `RenderSettings` (`colorLut`, `vignette`,
`filmGrain`) and Off by default; with all off the pass records nothing and the
post pass writes the output directly. Owns the output-sized RGBA16F display
target and the RGBA16F LUT texture, which it loads from the settings' `.cube`
path (relative to `RenderRuntime::assetRoot()`) only when the path changes.

## `src/engine/graphics/ColorGrading.*`

CPU reference for the grade: sRGB transfer functions, trilinear LUT sampling,
the vignette falloff, and the integer-hash grain. `color_grade.frag` mirrors it
by hand; `tests/engine/ColorGradingTests.cpp` covers it with the `.cube` parser.

## `src/engine/graphics/passes/ScreenSpaceReflectionPass.*`

Hi-Z screen-space reflections in compute (`shaders/ssr_trace.comp`,
`shaders/ssr_composite.comp`), run inside the scene pass after opaque
surfaces and the background and before blended surfaces, so blended surfaces
composite over them and TAA accumulates them. The trace reflects the view vector about the
prepass normal (GGX-jittered per frame when TAA accumulates), walks the
closest-depth channel of the depth pyramid, rejects hits that are too thick or
face away from the ray, and samples the current frame's lit scene at the hit.
It writes premultiplied radiance and confidence (screen-edge, reach, and
iteration-budget fades) at half or full render resolution into one shared
target. The composite mixes that radiance into the scene in place with a
per-channel Fresnel weight from the prepass specular reflectance (F0, so metals
reflect tinted) and a roughness fade, replacing that share of the
IBL/probe specular; misses leave the scene unchanged. Without the depth
prepass, reflections are skipped. `ScreenSpaceReflectionTrace.hpp` is the CPU
mirror covered by `tests/engine/ScreenSpaceReflectionTests.cpp`.

## `src/engine/graphics/passes/BloomExposurePass.*`

Compute passes over a generic HDR input (view, image extent, valid extent):
a 256-bin log-luminance histogram and single-workgroup adaptation into one
persistent exposure buffer per view, plus a six-level bloom chain (13-tap
downsample with a Karis average on the first level, tent upsample). The chain
and exposure buffer persist across frame slots and use push descriptors.

## `src/engine/graphics/ToneMapping.*`

CPU reference for the AgX/ACES display transform, histogram binning,
percentile averaging, and frame-rate independent exposure adaptation. The
shaders mirror it by hand; `tests/engine/ToneMappingTests.cpp` covers it.

## `src/engine/graphics/passes/MotionBlurPass.*`

Camera and object motion blur from the motion vectors, after the temporal
resolve and before exposure and bloom (`shaders/motion_blur_tile_max.comp`,
`motion_blur_neighbor_max.comp`, `motion_blur_reconstruct.comp`). Owns an
output-resolution RGBA16F blurred scene and two tile-velocity images per view
(not per frame slot), allocated on the first blurred frame after creation or
resize. Each frame reduces the per-pixel blur velocity to its tile's largest,
takes the largest of each 3x3 tile neighbourhood, and gathers 8 (Low) or 16
(High) depth-weighted samples along it (McGuire et al. 2012). Pixels without
geometry reproject the far plane, so camera rotation blurs the sky.

## `src/engine/graphics/MotionBlur.*`

CPU mirror of the motion blur plan: sample counts, the frame-rate independent
velocity scale (shutter fraction of a 60 Hz frame over the real frame time),
radius scaling for dynamic resolution, tile size and count, and the clamped
per-pixel blur velocity; `tests/engine/MotionBlurTests.cpp` covers it with the
settings round trip and fallback.

## `src/engine/graphics/passes/TemporalResolvePass.*`

Temporal anti-aliasing and upsampling (`shaders/taa_resolve.comp`). Owns two
output-resolution history images per view used ping-pong (not per frame slot),
allocated on the first TAA frame after creation or resize. It
reconstructs the jittered internal-resolution frame at output resolution,
reprojects Catmull-Rom history with dilated motion, rejects disoccluded
history, and blends after YCoCg variance clipping with luma weighting. Its CPU
mirror is in `TemporalQuality.*`. Each frame fills a top-left region of its
history image (the output extent, or the render extent ahead of FSR 1) and
reprojects the previous frame's region by UV, so region changes keep history.

## `src/engine/graphics/passes/SpatialUpscalePass.*`

AMD FidelityFX Super Resolution 1 for `UpscalerMode::Fsr1`. The tone-mapped
frame is composited at render resolution into the top-left of an output-sized
input target (the output format); `shaders/fsr_easu.comp` upsamples that
region into an RGBA8 gamma-2 image, and `shaders/fsr_rcas.frag` sharpens it in
a fullscreen draw into the output. Both targets are allocated on first use and
shared by the frames in flight. Unsupported formats leave the pass disabled,
and validation falls back to TAAU or native resolution.

## `src/engine/graphics/SpatialUpscale.*`

Upscaler presets (Quality 1.5x, Balanced 1.7x, Performance 2x), the
dynamic-resolution range a frame follows under an upscaler, the EASU constants,
and CPU references of EASU and RCAS that the shaders mirror;
`tests/engine/SpatialUpscaleTests.cpp` covers them with the upscaler settings'
serialization and fallbacks.

## `src/engine/graphics/passes/DepthOfFieldPass.*`

Thin-lens depth of field on the HDR scene, after the temporal resolve and
before exposure and bloom (`RenderSettings::depthOfField`). `dof_prepare.comp`
downsamples the scene to half resolution and stores each texel's signed circle
of confusion from the motion target's depth; `dof_gather.comp` gathers a
golden-angle bokeh spiral there (background samples limited so they never
spread over a sharper foreground); `dof_composite.comp` blends the upsampled
blur over the sharp full-resolution scene by the pixel's own CoC and the
foreground blur covering it. The gather targets and output persist across
frame slots in GENERAL and are allocated on first use.

## `src/engine/graphics/DepthOfField.*`

CPU reference for the depth of field: settings ranges, projection terms, view
distance from reversed-Z depth, the thin-lens circle of confusion in pixels
(focal length from the vertical field of view and sensor height), the gather
spiral's sample count, and the composite blend. The shaders mirror it by hand;
`tests/engine/DepthOfFieldTests.cpp` covers it with settings serialization and
the capability fallback.

## Upscaling

`RenderSettings::upscaler` selects how a frame rendered below output resolution
reaches it (`UpscalerMode`, serialized as `upscaler.mode`):

| Mode | Value | Behavior |
| --- | ---: | --- |
| Off | 0 | Historical behavior and the fallback: the internal resolution follows `dynamicResolution` alone, and TAA upsamples whatever it renders. Older documents without the key load as Off, so `antialiasing` 2 with dynamic resolution still means TAAU. |
| TAAU | 1 | Renders at the preset's scale and reconstructs with `TemporalResolvePass`. Requires TAA; validation enables it. |
| FSR 1 | 2 | Renders at the preset's scale, anti-aliases at render resolution (FXAA, or TAA resolving at render resolution), exposes, blooms, and tone maps there, then upsamples with AMD FidelityFX Super Resolution 1 EASU and sharpens with RCAS (`SpatialUpscalePass`). |

`upscaler.quality` picks the per-axis ratio: Quality 1.5x (0), Balanced 1.7x
(1), Performance 2x (2). `upscaler.sharpness` in [0, 1] sets RCAS: 1 is AMD's
maximum (0 stops) and each 0.5 below it halves the sharpening (default 0.9,
AMD's suggested 0.2 stops). With dynamic resolution enabled, its scale range
applies relative to the preset (both bounds are multiplied by the preset's
render scale), so the preset is the unloaded resolution; the internal extent
still varies inside fixed output-sized targets.

FSR 1 filters perceptual values, so it runs after tone mapping: `post.frag`
composites into the top-left render extent of an output-sized input target in
the output format (sRGB, which stores display values with 8-bit perceptual
precision), `fsr_easu.comp` gamma-2 encodes the display-linear values it reads,
upsamples, and stores them in an RGBA8 image, and `fsr_rcas.frag` sharpens that
encoding and decodes it in the fullscreen draw into the output. RCAS is a
fragment pass rather than compute so the swapchain needs no storage usage. HDR
values never reach EASU, so bright highlights cannot ring. The EASU taps are
fetched individually and clamped to the render extent, because texels beyond
it are stale under dynamic resolution. The editor grid composites at render
resolution and is upscaled with the scene.

The capability check (`RenderCapabilities::spatialUpscaling`) requires RGBA8
storage images and a sampled, renderable output format. Without it validation
falls back to TAAU where TAA is available and to native resolution otherwise,
each with a `fallbacks` message. A vendor upscaler such as DLSS would be
another `UpscalerMode` behind its own capability check; it needs a proprietary
SDK and is not included.

GPU scopes are `gpu.fsr_easu` (timestamps 47/48) and `gpu.fsr_rcas` (49/50);
`gpu.spatial_upscale` is their sum, checked against
`RenderPassBudgets::spatialUpscale` (serialized as `spatialUpscale`).
`render.spatial_upscale_bytes` reports the two targets once allocated. Both
scopes report zero unless the frame used FSR 1. `ZENITH_FORCE_UPSCALER` and
`ZENITH_FORCE_UPSCALER_QUALITY` select a mode and preset for diagnostics, and
`zenith_spatial_upscale` covers the presets, EASU constants and reconstruction,
RCAS, serialization, and fallbacks on the CPU.

## Motion blur

`RenderSettings::motionBlur` (`MotionBlurQuality`: Off, Low, High; JSON
`motionBlur` 0-2) blurs the HDR scene by the existing motion vectors after the
temporal resolve and before exposure, bloom, and tone mapping, so camera and
object motion (including skinned meshes) blur alike. Off is the default and the
fallback: no work is recorded, no images are allocated, and the post pass reads
the resolved (or raw HDR) scene as before; validation also selects it, with a
fallback message, when `RenderCapabilities::motionBlur` is false.
`motionBlurShutterFraction` (0-1, default 0.5) is the exposure as a fraction of
a 60 Hz frame; velocities are rescaled by the real frame time
(`motion_blur::velocityScale`), so the blur length follows on-screen speed, not
the frame rate. `motionBlurMaximumRadiusPixels` (1-64 output pixels, default 32)
caps the blur on either side of a pixel and sets the tile size. All three keys
are optional in the render-settings document.

The pass reduces blur velocities to tiles (`motion_blur_tile_max.comp`), takes
each tile's 3x3 neighbourhood maximum (`motion_blur_neighbor_max.comp`), and
gathers 8 (Low) or 16 (High) depth-weighted samples along it
(`motion_blur_reconstruct.comp`); pixels whose neighbourhood moves less than
half a pixel are copied. The scope is `gpu.motion_blur` (timestamps 43-44) with
the `motionBlur` budget; `render.motion_blur_bytes` reports the blurred scene
and tile images (17 MB at 1920x1080). `zenith_motion_blur` covers velocity
scaling, radius and tile math, and the settings round trip and fallback.

## Depth of field

`RenderSettings::depthOfField` (`DepthOfFieldSettings`, serialized as the
optional `depthOfField` object) blurs the HDR scene with a thin-lens circle of
confusion, after the temporal resolve and before exposure, bloom, and tone
mapping. `quality` (`DepthOfFieldQuality`) is Off (the fallback: no work, no
memory), Low, or High; both gather at half resolution, and Low spaces its
golden-angle spiral twice as far apart (about half the samples). The lens is
`focusDistance` (meters along the view axis), `fStop`, `sensorHeightMillimeters`
(with the camera's vertical field of view it fixes the focal length), and
`maximumCocRadiusPixels` (the blur radius limit in pixels of the blurred image,
which bounds the gather cost). Deserialization rejects values outside the
ranges in `DepthOfField.hpp`; validation turns depth of field Off with a
fallback message when the device lacks the capability or settings set in code
are out of range. `ZENITH_FORCE_HIGH_QUALITY` selects High and
`ZENITH_DISABLE_DEPTH_OF_FIELD` forces Off.

The GPU scope is `gpu.depth_of_field` (timestamps 41 and 42, zero when Off)
against `RenderPassBudgets::depthOfField` (`depthOfField` in the budgets
object); `render.depth_of_field_bytes` reports its targets (two half-resolution
and one output-resolution RGBA16F image, allocated on first use).
`zenith_depth_of_field` covers the CoC against the thin-lens formula, depth
linearization, the gather sample count, the composite blend, settings
serialization, and the fallbacks.

## Colour grading

`ColorGradePass` is a display-referred finishing stage after tone mapping. Its
three effects are independent `RenderSettings` members, each Off by default,
serialized under the render-settings `colorGrade` object (every object and
field optional, so older documents load with the stage off):

| Setting | JSON (`colorGrade.*`) | Range | Effect |
| --- | --- | --- | --- |
| `colorLut` | `colorLut.enabled`, `.path`, `.strength` | strength 0..1 | 3D `.cube` LUT applied to sRGB-encoded values, mixed over the ungraded image by `strength`. `path` is relative to the render asset root (the project's `assetRoot`) or absolute; empty applies nothing. |
| `vignette` | `vignette.enabled`, `.intensity`, `.smoothness`, `.roundness` | 0..1, 0.01..1, 0..1 | Corners keep `1 - intensity`; `smoothness` is the falloff width as a fraction of the centre-to-corner distance; roundness 1 is circular on screen, 0 follows the frame's aspect. |
| `filmGrain` | `filmGrain.enabled`, `.intensity`, `.size` | 0..0.5, 1..8 px | Uniform grain added in sRGB-encoded units over cells of `size` output pixels, re-seeded every frame by the frame serial, so `ZENITH_FRAME_DETERMINISTIC` captures repeat it exactly. |

With every effect off (or only a LUT whose file failed to load, which is
reported once on stderr) the post pass tone-maps straight into the output and
the stage records nothing: the Off fallback costs nothing and leaves images
bit-identical. Otherwise tone mapping writes an output-sized RGBA16F display
target and one fullscreen draw (`shaders/color_grade.frag`) writes the output.
The LUT is parsed on the CPU (`engine/assets/ColorLut`) and uploaded as an
RGBA16F 3D texture with linear filtering only when its path or the asset root
changes. `ColorGrading.hpp` is the CPU reference; `zenith_color_grading` covers
the parser (sizes, domains, malformed input), sampling, the vignette, grain
determinism and the settings document. `assets/color_grading/warm_cinematic.cube`
(17^3, regenerated by `tools/color_grading/`) is an example grade.

The grade is deliberately not part of `ZENITH_FORCE_HIGH_QUALITY`: it is an
artistic choice of the project, not a quality tier, so forcing it would change
every reference image rather than raise fidelity.

The scope is `gpu.color_grade` (timestamps 45/46) with the budget
`RenderPassBudgets::colorGrade` (`budgets.colorGrade`). Most of the cost is the
extra full-screen RGBA16F write and read, so it scales with output pixels;
`gpu.post` itself is unchanged. LUT files up to the specification's 256^3 load
(about 130 MB as RGBA16F), but they are parsed and uploaded on the render
thread when the path changes, so practical grades stay at 17^3 to 65^3.

## Post-process descriptors and FXAA

`post.frag` binds 0 HDR scene, 1 depth, 2 temporally resolved scene, 3 bloom
mip 0 and 4 the exposure buffer; the former placeholder bindings for motion,
ambient occlusion, reflections and previous motion are gone. FXAA operates on
the composited image: every FXAA tap re-evaluates the same composite as the
centre (scene plus editor grid), so grid lines are antialiased with the scene
instead of being blended against grid-free neighbours, and taps stay one
scene-image texel apart. With the grid off and full resolution the result is
unchanged. TAA frames do not run FXAA.
