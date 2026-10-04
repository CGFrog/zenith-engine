# Viewports (`panels/Viewport*` and `src/editor/viewport/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`panels/ViewportPanel` is one 3D viewport area; a workspace may show any
number. Each owns its `EditorCamera`, transform tools, chrome state, and a
render view, and draws Blender-style chrome over its image as ImGui child
windows, so clicks on it never reach picking, the gizmo, or the camera: a
header with View (regions, camera view), Shading, Overlays, and Gizmos menus,
the `T` tool shelf on the left, and the `N` sidebar on the right with Item
(active transform), Tool (transform tool and snapping), and View (camera field
of view, clip planes, fly speed) tabs plus the tabs added to every viewport
with `ViewportRegistry::addSidebarTab`.

`panels/ViewportRegistry` is the session-owned record of the viewports. A
viewport registers in its constructor and unregisters in its destructor; it
takes a render view the first time it is drawn (the renderer's default view
when free, otherwise `createView`) and returns it when destroyed, so closing
an area destroys its view. A viewport is open while it was drawn in the
previous frame; the active viewport is the one last hovered or clicked while
open, otherwise the first open one, otherwise none. The session makes the
active viewport's view the renderer's active view (shadow cascades and
streaming follow it), and modal transforms, the Adjust Last Operation anchor,
snapping, and the status bar use it. A modal transform keeps the viewport it
started in and is cancelled if that viewport closes. `panels/ViewportOperators`
registers every `view3d.*` operator (chrome toggles, `shading_set`,
`toggle_snapping`, viewpoints, orbit, framing, `view_camera`) against the
active viewport; with none open they poll false and fail descriptively.

Viewport shading never touches the project's render settings: each draw hands
the viewport's view `viewportViewSettings` (a copy of the project settings
with the viewport's `ViewShading`, reduced by `viewShadingFrameSettings`
for the device) through `RenderRuntime::setViewSettings`, rebuilt every frame
so project changes flow through, and nothing in Play mode
(`viewportShadingSource`: Play, also paused, shows the project's look; Edit
and Simulate keep the viewport's shading). A viewport can look
through a scene `CameraComponent` (Numpad 0, or View > Look Through): the
view is overridden with that camera's lens, navigation is disabled, and any
navigation operator or View > Exit Camera View returns to the editor camera.
`panels/ViewportChrome` holds the per-viewport chrome state
(`ViewportChromeState`: region visibility, `ViewportShading`, overlays) and
its widgets. `EditorCamera` owns the lens (field of view and clip planes) that
its render view, projections, and picking share; `lensViewMatrix`,
`lensProjectionMatrix`, `pickThroughLens`, and `sceneCameraLens` do the same
math for any `RenderView`. `RenderRuntime::setViewVisibleEntities` isolates a
render view to a set of objects for local view (a default view falls back to
CPU submission while isolated); its sun, point, and spot shadows then cast
from that set only, through its own shadow maps when another view needs the
whole scene's (`tests/engine/IsolatedShadowSmokeTests.cpp`). `tests/editor/ViewportChromeTests.cpp` and
`tests/editor/MultiViewportTests.cpp` cover this headless.

`viewport/SceneGizmoDraw` records a viewport's scene overlays into its own
render view's debug list (`RenderRuntime::viewDebugDraw`), never the all-views
list, so nothing reaches another view: light shapes (point range spheres, spot
inner and outer cones along the transformed direction, directional arrows
along the light's travel, environment markers), camera frustums at the view's
aspect (the active primary camera highlighted, a selected one out to its far
plane, the camera being looked through skipped), collider shapes from
`ColliderComponent` (physics only steps in Play, so the editor does not use
`PhysicsWorld::drawDebug`; shapes follow the body construction with world
scale baked in), reflection probe, decal (oriented, scaled, with a -Y
projection arrow) and irradiance volume (axis-aligned) bounds, world-space
selection bounds, and name labels. It has no ImGui, takes `SceneGizmoOverlays`
(each category `Off`, `Selected`, or `All`; colliders and names default to the
selection), skips entities `SceneVisibility` hides in the editor and, in local
view, those outside the viewport's isolation (`SceneGizmoView::isolatedObjects`),
bounds a selected collection instancer by the members it draws, reads each
mesh's cached `MeshAsset::localBox` instead of scanning vertices, and colours
with the palette's `overlay*` tokens. `ViewportPanel::syncRenderView` records
it every frame (always in Edit; in Play only with **Show in Play Mode** and
never through a scene camera, `viewportDrawsSceneGizmos`) and draws the labels
the view projected last frame (`viewDebugLabels`) with ImGui over the image.
The Overlays menu, `view3d.overlay_toggle {overlay}`, and
`view3d.overlay_scope_set {overlay, scope}` change them; the viewport saves
its overlays as its area state (`viewportAreaState`).
`tests/editor/SceneGizmoDrawTests.cpp` covers the geometry and policy on the
CPU; `tests/editor/SceneGizmoCaptureTests.cpp` is the strict-validation smoke
test and the `scene_gizmos` reference capture.


`src/editor/viewport/` holds the viewport's selection tools. A left click runs
`view3d.select` (Shift extends, a miss clears unless extending; Alt runs
`view3d.select_cycle`, stepping from the active object through the objects
under the cursor, visible ones first, then occluded ray hits), B starts the
modal box tool (`view3d.select_box`, `BoxSelect`), and Ctrl+RMB drags a lasso
(`view3d.select_lasso`, `LassoSelect`; Ctrl+Shift+RMB extends, Ctrl+Alt+RMB
subtracts). Box and lasso take `auto` (Shift extends, Ctrl subtracts at
release), `set`, `extend`, or `subtract`; Esc or a right click cancels them.
`ViewportPicker` resolves every pick through the GPU id buffer when it can and
the CPU otherwise; hidden and unselectable objects are never picked, a
collection instance picks its instancer, and an armed eyedropper takes a click
instead of the selection. Numpad / (`view3d.localview`) toggles local view per
viewport: the selection and its descendants are isolated in that viewport's
render view (and its picks) and framed, and toggling again restores the
previous camera. `RegionSelect` holds the shared modes and outline drawing,
`ViewFraming` the framing math shared with `view3d.frame_selected`, and
`ViewportSelectOperators` the operators. `tests/editor/ViewportPickingTests.cpp`
covers clicks, the pending-pick fallback, box, lasso, cycling, the eyedropper,
and local view headless with a synthetic `ViewportPickProvider`; the editor
smoke clicks an object through the real id buffer and toggles local view.

Snapping, pivot points, and the 3D cursor. Each viewport's
`TransformToolSettings` (`panels/ViewportChrome`) hold the pivot point
(`TransformPivot`: Median Point, Active Element, Individual Origins, 3D Cursor;
`transform.pivot_set {pivot}`), the snap target (`SnapTarget`: Increment,
Surface, Vertex; `transform.snap_target_set {target}`), Absolute Grid Snap,
and the increments, chosen in the Gizmos menu or the sidebar's Tool tab and
saved in the viewport's area state under `"tools"` beside `"overlays"`. Modal
G/R/S (`interaction/ModalTransform`, driven by
`application/EditorObjectCommands`) and the gizmo use them: rotation and
scale turn about the pivot (Individual Origins turns each object in place);
translation snaps the selection's median (or active origin) in increments
from the start, onto the absolute grid, or onto the surface or vertex under
the mouse, projected onto an axis or plane constraint; Ctrl inverts snapping
while held. The gizmo sits on the cursor while rotating or scaling about it,
and snaps its translation along the axes being dragged. `viewport/SnapQueries`
finds targets on the CPU against pickable meshes, skipping the moved objects
and their descendants (the id buffer holds only the front-most surface, which
during a snapping drag is the moved object, and arrives frames late; physics
bodies only exist while simulating). `viewport/Cursor3D` places, draws, and
operates the cursor: Shift+RMB in a viewport runs `view3d.cursor3d_place {x,
y}` (the surface under the mouse, or the cursor's depth along the view ray);
`view3d.cursor_reset`, `view3d.snap_cursor_to_selected`, and the undoable
`view3d.snap_selected_to_cursor {offset}` sit in View > Snap. Every viewport
records the cursor into its view's debug list (`overlays.cursor`, the
`palette::cursor*` tokens). The cursor belongs to the scene:
`EditorDocument::editorData()` (`document/SceneEditorData`) reads it from
the sidecar `<scene file>.editor.json` on open, writes it on save (removing
the sidecar while the cursor is at the origin), and resets it for a new scene;
it is not part of undo. New objects spawn at the cursor: the add operators and
`prefab.instantiate` fill their `location` from it through
`Operator::invokeDefaults` (so Adjust Last Operation shows and edits it, and
an explicit location wins), and dropped models go to it
(`EntityCommands::placeModel`). `tests/editor/SnappingCursorTests.cpp` covers
the pivot and snap math, the queries against real cube meshes, the operators,
spawning, drawing, area state, and the sidecar headless; the editor smoke
(frame 310) places the cursor, adds an Empty at it, and undoes.

## `src/editor/interaction/PickQueries.*`

Pure selection queries over a GPU pick result's id grid: point picks with a
pixel tolerance, box and lasso (even-odd, pixel centres) selection, depth at a
pixel, and Alt+click cycling candidates merged with CPU ray hits. The header
documents how a viewport turns clicks and drags into `requestPick` regions
(implemented by `editor/viewport/ViewportPicker`);
`tests/editor/PickQueriesTests.cpp` covers each query on synthetic grids.

## Folder contents

`src/editor/viewport/`: 3D viewport tools: scene overlays (`SceneGizmoDraw`:
light, camera, collider, volume, and selection gizmos and name labels recorded
into a view's debug list, free of ImGui so they also run in engine-hosted
tests) and selection (GPU-first picking with CPU fallback through
`ViewportPicker`/`ViewportPickProvider`, the modal box and lasso tools, view
framing, and the select and local view operators), snap target queries
(`SnapQueries`), and the 3D cursor (`Cursor3D`: placement, drawing, and its
operators).
