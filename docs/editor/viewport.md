# Viewports (`panels/Viewport*` and `src/editor/viewport/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`panels/ViewportPanel` is one 3D viewport area; a workspace may show any
number. Each owns its `EditorCamera`, transform tools, chrome state, and a
render view, and draws Blender-style chrome over its image as ImGui child
windows, so clicks on it never reach picking, the gizmo, or the camera: a
header with View (regions, camera view), Shading, Overlays, and Gizmos menus,
the `T` tool shelf on the left, and the `N` sidebar on the right with Item
(active transform), Tool (transform tool and snapping), View (camera field
of view, clip planes, fly speed), and Navigation tabs plus the tabs added to
every viewport with `ViewportRegistry::addSidebarTab`. The Navigation tab
(`viewport/NavigationSidebarTab`) bakes and clears the preview navigation
mesh (`navigation.bake`, `navigation.clear`, also in Object > Navigation),
shows its stats and the project settings it bakes with, opens them
(`project.settings {tab: navigation}`), toggles the viewport's **Navigation
Mesh** overlay (`overlays.navigationMesh`, which gates the preview's drawing
and is saved with the other overlays), and in Play shows the game's mesh and
agent count through `OutsideServices::navigationIfBaked`, which never bakes.

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

Wireframe shading draws only the edges of every surface, through each other
(Blender's default x-ray wireframe), on the dark scene colour: the surfaces are
not shaded and write no depth, so selection outlines and the grid still draw.

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
along the light's travel, environment markers), camera icons (a small
fixed-size pyramid with Blender's up triangle, the same for every camera and
independent of the lens and the window's aspect, so cameras stay easy to see
and click; the active primary camera highlighted; a selected one adds dotted
rays along its view at its field of view, up to `cameraRayLength` or its far
plane, instead of a frustum; the camera being looked through skipped), collider shapes from
`ColliderComponent` (physics only steps in Play, so the editor does not use
`PhysicsWorld::drawDebug`; shapes follow the body construction with world
scale baked in and the collider's offset applied through
`colliderLocalTransform`), reflection probe, decal (oriented, scaled, with a -Y
projection arrow) and irradiance volume (axis-aligned) bounds, world-space
selection bounds, and name labels. It has no ImGui, takes `SceneGizmoOverlays`
(each category `Off`, `Selected`, or `All`; names default to the selection).
Nothing is visible only while selected: unselected colliders (a triangle mesh
as its bounds only) and directional lights (the sun) draw faintly, at
`palette::overlayIdleAlpha`, and a selected one takes the bright selection
colours over the scene. The Colliders category also draws each character
controller's capsule, centred on the entity and upright, and a convex hull
draws the mesh it wraps. It skips entities `SceneVisibility` hides in the editor and, in local
view, those outside the viewport's isolation (`SceneGizmoView::isolatedObjects`),
bounds a selected collection instancer by the members it draws, reads each
mesh's cached `MeshAsset::localBox` instead of scanning vertices, and colours
with the palette's `overlay*` tokens. `ViewportPanel::syncRenderView` records
it every frame (always in Edit; in Play only with **Show in Play Mode** and
never through a scene camera, `viewportDrawsSceneGizmos`) and draws the labels
the view projected last frame (`viewDebugLabels`) with ImGui over the image.
The Overlays menu, `view3d.overlay_toggle {overlay}`, and
`view3d.overlay_scope_set {overlay, scope}` change them; the viewport saves
its overlays as its area state (`viewportAreaState`). Below them, a **Debug
Draw** section (`drawDebugDrawChannelsMenu`) lists the named channels of the
gameplay and module debug draw (`RenderRuntime::debugDraw`, `DebugDraw::
channels`) that hold primitives or are hidden, each as a toggle; hiding a
channel hides it in every view, game views included, and is not saved.
`tests/editor/SceneGizmoDrawTests.cpp` covers the geometry and policy on the
CPU; `tests/editor/SceneGizmoSmokeTests.cpp` is the strict-validation smoke
test and the `scene_gizmos` reference capture.


Play's debugging overlays. In Play and Simulate the Overlays menu's Play
section (also View > Overlays in the main menu, `debugOverlayItems`) turns
on overlays that look at the running game rather than the authored
components: **Physics Shapes** (`PhysicsWorld::drawDebug`), **Contacts** (the
last step's contact points and normals), **Characters** (character capsules,
green grounded with the ground normal, blue airborne), and **Nav Agents**
(each agent's radius, crowd velocity and path end, yellow for a partial path
and red for a failed one); **Navigation Mesh** then shows the game's own mesh
(`OutsideServices::navigationIfBaked`, which never bakes) instead of the
preview. `viewport/RuntimeDebugDraw` records them without ImGui
(`recordRuntimeDebug`), `ViewportPanel::recordRuntimeOverlays` calls it every
Play frame whatever **Show in Play Mode** says, game views included, and the
flags are saved with the other overlays. They start off; colours are the
palette's `overlayContact`, `overlayCharacter*`, and `overlayNav*` tokens.

Collider editing. The inspector's collider editor
(`ComponentEditors::drawCollider`) switches the shape type in a Shape combo,
keeping its size (`convertColliderShape`: a box's largest half extent becomes
a sphere's radius, a capsule keeps the box's height and wider side, and so
on), edits the offset position and rotation (Euler degrees), and gives a
triangle-mesh or convex-hull collider a combo of the entity's model meshes
(`colliderMeshChoices`; the mesh shapes are unavailable without one). While the
Colliders overlay is not Off, `viewport/ColliderHandles` draws handles on the
active object's collider: a square on each box face, on a sphere's radius
along each axis, and on a capsule's radius and the tips of its ends, placed
with `colliderLocalTransform`. Dragging one moves it along its outward axis
(the mouse ray's nearest point on that line,
`colliderHandleAxisParameter`): symmetric by default, so the shape grows on
both sides and stays centred, and one-sided with Ctrl, which grows it by half
the distance and moves the offset after the dragged side
(`dragColliderHandle`; sizes stay at least 1 mm). The drag is one undo step,
captured at the press and committed at the release like the gizmo; Esc or a
right click cancels it. The transform gizmo takes the mouse first, a hovered
or dragged handle blocks picking, and the handles are skipped in game views
and during modal transforms (which cancel a drag).
`tests/editor/ColliderToolsTests.cpp` covers the shape switch through the
inspector, the mesh choices, the handle placement and drag maths, and a drag's
undo and cancel headless.

`src/editor/viewport/` holds the viewport's selection tools. A left click runs
`view3d.select` (Shift extends, a miss clears unless extending; dragging with
the left button draws a box (`view3d.select_box` with `left_drag`, started
where the press began; Shift extends, Ctrl subtracts at release), the Select
Box behaviour of Blender's default tool; Alt runs
`view3d.select_cycle`, stepping from the active object through the objects
under the cursor, visible ones first, then occluded ray hits), B starts the
modal box tool (`view3d.select_box`, `BoxSelect`), and Ctrl+RMB drags a lasso
(`view3d.select_lasso`, `LassoSelect`; Ctrl+Shift+RMB extends, Ctrl+Alt+RMB
subtracts). Box and lasso take `auto` (Shift extends, Ctrl subtracts at
release), `set`, `extend`, or `subtract`; Esc or a right click cancels them.
`ViewportPicker` resolves every pick through the GPU id buffer when it can and
the CPU otherwise; hidden and unselectable objects are never picked, a
collection instance picks its instancer, and an armed eyedropper takes a click
instead of the selection. Objects without a mesh (lights, cameras, probes,
decals, irradiance volumes, audio emitters and listeners, particle emitters,
fog) are clicked by their overlay marks: `IconPicking::pickableIcons` lists
the origins `SceneGizmoDraw::sceneGizmoAnchors` marks (same scopes,
visibility, and isolation as the drawn overlays) plus the emitters', the
query carries them (`ViewportPickQuery::icons`, empty when the view draws no
overlays), and `mergeIconPicks` tests them in screen space before the mesh
result: a click within `iconPickRadiusPixels` picks the nearest icon over the
mesh behind it unless a pickable surface along the click's ray is nearer
than the icon (the default scene's sun inside the cube does not steal the
cube's clicks), Alt+click cycling lists the visible icons first and hidden
ones after the meshes, and box and lasso add the icons inside. Numpad / (`view3d.localview`) toggles local view per
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
while held. The gizmo (`viewport/MultiGizmo`) moves every selected object
(the selection's transform roots, so a parent and child move once): it is a
frame with unit axes on the pivot point (translation uses the median, or the
active origin, whatever the pivot), oriented by the active object in Local
space and by the world in World space. Each frame `decomposeGizmoChange`
splits the change since the drag began into a translation, a world rotation,
and a per-axis scale along the gizmo's axes, and `applyPivotedTransform`
(shared with the modal transforms) applies it to the objects' start matrices,
scaling each along its own axes so rotated objects never shear. A drag is one
undo step (`captureEntities` of the dragged roots, committed on release); in
Play it edits the running scene unrecorded. The gizmo snaps its translation
along the axes being dragged. `viewport/SnapQueries`
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
an explicit location wins). Models and prefabs dropped from the Asset
Browser land under the mouse instead (`viewport/ViewportDrop`):
`placementUnderMouse` (`SnapQueries`) casts the mouse ray against the
pickable meshes and returns the hit and its surface normal, or over empty
space the point at the cursor's depth (`placementDepth`), and the drop runs
`object.place_model` or `prefab.instantiate` with that `location` and
`normal`, so Adjust Last Operation can move it or toggle `align_to_normal`.
A game view takes no drops. `LensHit::normal` is the hit face's world normal
on its front side (counter-clockwise winding), transformed by the inverse
transpose so it stays outward under negative scale; `SurfaceHit` turns it to
face the ray. `tests/editor/MultiGizmoTests.cpp` covers the
pivot maths and a multi-object drag's single undo step,
`tests/editor/PlacementTests.cpp` hit normals and placing under the mouse,
`tests/editor/SelectionToolsTests.cpp` icon picking, and
`tests/editor/SnappingCursorTests.cpp`
the pivot and snap math, the queries against real cube meshes, the operators,
spawning, drawing, area state, and the sidecar headless; the editor smoke's
`3D cursor` step places the cursor, adds an Empty at it, and undoes.

## Renderer contract

The editor requests a viewport size through `RenderRuntime`; it never receives
Vulkan images, views, samplers, or descriptor sets. `Renderer` owns a
per-frame offscreen color target plus its depth target and exposes only an
opaque `UiTextureHandle`. Viewport resize recreates those targets and the
size-dependent mesh/post-process targets without recreating the swapchain.
Descriptor replacement happens before the next ImGui frame so queued UI draw
data cannot retain a destroyed texture. Further editor viewports are created
render views (see [Render views](../engine/graphics-views.md#render-views))
with the same contract: every viewport area owns one view for its lifetime
(the first takes the default view), renders only on frames it is drawn, and
sets its shading through a per-view settings override, never the frame
settings. The editor makes the active viewport's view the renderer's active
view, so shadow cascades follow the view being worked in.

UI images follow the same request-and-publish contract for CPU pixels the UI
draws, such as generated images: `RenderRuntime::createUiImage` and
`updateUiImage` queue RGBA8 pixels with a linear or nearest filter, the
renderer's `UiImageStore` uploads them while recording the next frame, and
`uiImageTexture` then returns an opaque `UiTextureHandle`. An updated image
keeps its previous texture until the new one is published, and replaced or
destroyed images are released only after the frames that may sample them
complete.

Editor camera policy, selection, and gizmo state remain in `src/editor/`.
`RenderView` is the narrow engine-facing camera override. The headless picking
path is a bounded CPU ray test against transformed mesh bounding spheres and
triangles (`pickThroughLens`); a deterministic test
(`tests/editor/EditorCameraTests.cpp`) exercises it with 2,000 additional
renderables and reports its elapsed time without a machine-dependent timing
assertion. GPU picking ([Picking](../engine/graphics-views.md#picking)) is the
windowed path.

Play in a viewport needs no renderer support beyond that contract: the
playing viewport clears its view's camera override, so the view renders
through the scene's primary `CameraComponent` (the renderer's fallback, which
the game's camera systems move), and turns its view input on only while the
game view has focus. The editor application answers
`Application::mouseCapturePolicy()` from the session each frame: `Persistent`
while a game view holds game input (the engine's input releases the cursor on
Esc), `WhileRightMouseDown` otherwise. The standalone target crosses a process
boundary instead of a library one: the game's `src/main.cpp` owns the
`--project <file> --scene <scene>` argument contract, and the editor only
reads the process's output (see [Play mode](play-mode.md)).

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
`ViewportPicker`/`ViewportPickProvider`, clicking mesh-less objects by their
marks in `IconPicking`, the modal box and lasso tools, view framing, and the
select and local view operators), the multi-object gizmo maths
(`MultiGizmo`), snap target and placement queries (`SnapQueries`), asset
drops (`ViewportDrop`), and the 3D cursor (`Cursor3D`: placement, drawing,
and its operators).
