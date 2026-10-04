# Graphics post-processing

Part of the [repository guide](../REPOSITORY_GUIDE.md).

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
