# Graphics render views and picking

Part of the [repository guide](../REPOSITORY_GUIDE.md).

How one frame renders several views of a scene, how a view can draw a
secondary scene, and how GPU picking reads object ids back from a view. The
types live in `src/engine/graphics/Rendering.*` (`RenderRuntime`),
`RenderViewPlanning.*`, `RenderPicking.*`, and `passes/MeshPass*`; see
[Graphics](graphics.md) for the files and [Graphics scene passes](graphics-passes.md)
for the passes a view records.

## Render views

A frame can render several views of one scene, each with its own camera, size,
and history. `RenderRuntime` keeps the view registry: the default view always
exists (the single-viewport calls `requestViewportSize`, `viewportTexture`,
`setViewOverride`, `setGridOverlay`, `setOutlinedObjects`, and
`setViewportInputActive` address it), and `createView`/`destroyView` hand out
generational `RenderViewId`s for more. Per view the application sets a size, a
camera override (`RenderView`: position, orientation, vertical FOV, near and
far planes), a grid, outlines, input focus, and optionally a settings override;
the renderer publishes the view's texture, size, and `RenderViewStats`.
`composeViewSettings` decides what a view may override: anti-aliasing, the
upscaler and dynamic resolution, AO, reflections, fog, bloom, exposure and tone
mapping, depth of field, motion blur, colour grading, decals, the depth
prepass, lighting quality, and LOD threshold are per view, so a cheap view can
skip all temporal and screen-space work. Shadows, environment and probe
quality, indirect diffuse, local lights and their shadows, particles,
GPU-driven submission, and streaming stay frame settings.

### Secondary scenes

A created view may also draw a **secondary scene** instead of the
application's: `createScene` hands out a generational `RenderSceneId`,
`setViewScene` binds a view to it (never the default view), and each frame the
owner binds the scene's content with `setSceneContent` before `drawFrame`. The
editor uses this for prefab windows. A secondary scene has its own render
packets and caches in `MeshPass` (`SceneRenderState`, exchanged into the
per-scene members while that scene builds and records; scenes are keyed by
handle because revision counters start alike in every scene), its assets are
leased into the frame's leases, and it is drawn with preview lighting: the
studio rig, CPU submission, and none of the frame stage's sun cascades,
local-light shadows, environment maps, probes, irradiance volume, fog, or
particles, which belong to the application's scene. Its views follow every
view of the application's scene in a frame, and it always honours the editor's
viewport hide. The cost of a secondary scene is in
[Graphics](graphics.md#srcenginegraphicsrendering).

### Visibility filtering and per-view shadows

What renders is filtered twice. Render packets include only entities the
runtime's `SceneVisibilityMode` draws (`SceneVisibility`: `renderable` flags
always, the editor's viewport hide only in `Editor` mode, which the editor
sets while editing and clears in Play mode, so games never see it), plus the
draws `expandCollectionInstances` produces for collection instances; light and
particle gathering apply the same filter, and so does the scene-wide lighting:
the frame's sun, environment, and fog (`frameDirectionalLight`,
`frameEnvironmentLight`, `frameVolumetricFog` in `Lighting.hpp`) and the bound
irradiance volume come from drawn primary components only. A scene without a
primary sun or environment keeps the defaults; one whose primary components
are all hidden or not renderable gets none (a zero-intensity sun without
shadows, a black sky).

A view may then be isolated to a set of objects (`setViewVisibleEntities`, the
editor's local view), which filters its CPU-submitted draws and its picks; a
collection-instance draw belongs to its instancing entity. GPU-driven
submission draws every record, so an isolated default view uses CPU
submission. Shadows follow each view's isolation. The frame's sun cascades
(placed by the leading, active view's camera) and its local-light atlas live in
the frame's camera sets, so they cast from the isolation of the view drawing
through those sets: the default view, else the leading one. A view isolated
differently (a local view beside a full viewport, or a full view beside an
isolated default view) draws its own maps (`MeshPass::ViewShadows`): cascades
with the same placement and a local-light atlas for its own lights, both from
its casters only and cached like the frame's (revisions, caster set, light
matrices; a skipped frame redraws the atlas). They are created when first
needed (about 25 MB of cascades plus the atlas quality's size, and host object
records), retired through the release queue once the view samples the frame's
maps again, and reported in `RenderViewStats` (`ownShadowMaps` and the view's
shadow counts). A view without isolation costs nothing extra. Light lists and
particles still cover the whole filtered scene. Instance draws compose several
entities' transforms, so while any exist a transform or skin change rebuilds
the render packets instead of updating them in place.

### Recording order

The default view renders into the swapchain until a size is requested and
offscreen every frame after that. A created view renders only on frames that
called `requestViewSize` for it, so hidden panels cost nothing. The Renderer
orders the frame's views with the active view (`setActiveView`) first when it
renders, else the default view first (`RenderViewPlanning.*`); that leading
view places the shared work:

```text
MeshPass::record
  frame stage (once): render packets and uploads, materials, texture streaming
    (leading camera's priority), environment and probe residency, irradiance
    volume, light gathering, local-light shadow tiles for the leading view's
    lights, GPU-driven preparation (when the default view leads), directional
    shadow cascades fitted to the leading view's frustum
  each view, in order: camera buffer, light/probe/decal clusters, fog, CPU
    visibility, (leading view: particle simulation and sort), prepass, depth
    pyramid, AO, shading, SSR, transmission, particles, outlines, post chain
    into the view's target
```

### View state

Each view owns a `MeshPass::ViewState`: per-frame-slot camera sets (uniform
buffer and an object buffer for its own CPU-submitted draws), its clustered
lighting, probe and decal clusters, volumetric fog volumes and history, depth
pyramid, AO targets, transmission background, and a `PostProcessPass` with its
HDR, motion, normal and specular targets, TAA and SSR history, exposure, bloom,
depth of field, motion blur, FSR, and colour-grade targets. Views of different
sizes therefore never invalidate each other's history. The default view reuses
the frame's camera sets, whose object buffer also holds the shadow regions; a
created view's sets copy the shared bindings (shadow map, skin palettes,
materials, environment, irradiance volume, local-shadow atlas) from the
frame's set of the same slot before it records. GPU-driven submission and its
occlusion history belong to the default view. A view that does not lead
reuses the cascades, particle sort, probe residency, and local-shadow tiles of
the leading view; its own light selection finds those tiles by stable light
id. A view with its own shadow maps copies every shared binding but the shadow
ones, which point at its maps. Only the leading view writes GPU timestamps, so
pass timings describe the frame stage plus that view (other views count toward
the frame total).

### Target lifetime and tests

Creating a view also creates its passes' pipelines through the pipeline cache.
Its targets are created, resized, or destroyed at `Renderer::beginUiFrame`,
after one device idle when an existing view changes, so the UI draw data of
the frame that released a view can still sample its texture. The renderer's
per-view stats add up the views' draws; `RenderViewStats` reports each view's
own counts and CPU recording time. `tests/engine/RenderViewTests.cpp` covers the
registry and planning; `tests/engine/MultiViewSmokeTests.cpp` renders two
created views beside the default one under strict validation, isolating views,
switching the visibility mode, and moving a collection instance;
`tests/engine/IsolatedShadowSmokeTests.cpp` checks through the per-view shadow
counts that an isolated view's sun, point, and spot shadows cast from its
objects only while the view beside it keeps the whole scene's;
`tests/engine/TwoSceneSmokeTests.cpp` renders a secondary scene beside the
application's scene and picks in it.

## Picking

`RenderRuntime::requestPick(view, rect)` returns a `PickTicket` for the object
ids and depth under a rectangle of a view's output pixels
(`engine/graphics/RenderPicking.hpp`). The request waits in the runtime's
`PickRequestBook` until the view next renders; that frame `MeshPass` records
`IdBufferPass` once per request, right after the view's CPU visibility, and the
pass is skipped entirely on frames without requests (`RenderStats::pickPasses`
counts it). The id buffer draws the view's frustum-visible render items, the
same list CPU and GPU-driven submission draw from (so it inherits their
filtering), with the view's camera unjittered at its output aspect, skinned
poses from the frame's palettes, and reversed-Z depth; alpha-masked cut-outs
pick as solid and particles are not pickable. Each entity gets one id per
request (`id n` names `PickResult::objects[n-1]`, holding its `SceneEntityId`
and `RenderObjectId`; 0 is "none"). The region renders into the top-left of
targets that grow to the largest request (the viewport is offset by the
region's origin) and is copied with `IdReadback` into a host-visible buffer.

The Renderer keeps each readback until the frame that recorded it completes,
checking frame fences without waiting, then publishes the ids, the depth plane,
the object table, and the camera (`viewProjection` and its inverse, for
`pickWorldPosition` and other depth-to-world reconstruction). Results are
typically Ready one to three frames after the request and are taken once with
`pickResult`. A view destroyed or resized before its pick completes resolves it
to `Cancelled`, as does `cancelPick`; a failed recording resolves to `Failed`.
The book holds at most 64 tickets (`PickRequestBook::maximumOutstanding`) and
evicts the oldest untaken finished result when full.

Selection policy stays in the editor: `editor/interaction/PickQueries.*` holds
the pure point (with tolerance), box, lasso, depth, and Alt+click cycling
queries over a result's id grid, with the viewport integration contract in its
header. `editor/viewport/ViewportPicker` drives one viewport pick: it requests
the id buffer on mouse press, applies the result the frame it arrives, and
answers on the CPU (`pickThroughLens`, `pickAllThroughLens` for occluded
cycling candidates, projected bounds centres for box and lasso) at once when
the view does not render, or when the GPU pick fails, is cancelled, or is still
pending after a short frame budget (see [Viewports](../editor/viewport.md)). It
reaches the renderer through a `ViewportPickProvider`, which tests replace with
a synthetic id grid (`tests/editor/ViewportPickingTests.cpp`).
`tests/engine/RenderPickingTests.cpp` and `tests/editor/PickQueriesTests.cpp`
cover the CPU side; `tests/engine/PickingSmokeTests.cpp` picks known objects
through both submission paths under strict validation.
