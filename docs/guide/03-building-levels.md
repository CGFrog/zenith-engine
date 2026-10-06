# 3. Building a level

[Guide contents](README.md) · Previous: [The editor at a glance](02-editor-tour.md) · Next: [Playing and debugging](04-play-and-debug.md)

A Zenith level is a **scene**: a set of **entities**, each carrying
**components**. An entity on its own is just a name, a transform, and a place
in the hierarchy. Components give it everything else: a mesh, a light, a
collider, a behaviour. This chapter builds a small level by hand.

## Scenes

| Action | How |
| --- | --- |
| New scene | **File > New** (`file.new`). |
| Open a scene | **File > Open...**, or double-click a `.zenith-scene` in the Asset Browser. |
| Save | `Ctrl+S` (**File > Save**). Save beneath the project's scene root. |
| Save as | **File > Save As...** |

Saves are atomic (a crash never leaves half a file). While a scene has unsaved
changes, a recovery copy is written periodically and offered after an
interrupted session. New, Open, Quit, and the window's close button ask what
to do with unsaved changes; **Cancel** always leaves your scene untouched.

A scene file is JSON (`.zenith-scene`). It refers to assets by GUID *and*
path, so renaming or moving an asset in the Asset Browser never breaks it. The
editor's viewport-only state (the 3D cursor) lives in a small
`<scene>.editor.json` beside it.

## Adding objects

Press `Shift+A` over the viewport (or **Edit > Add**) for the Add menu:

- **Mesh > Cube**, **Light > Sun / Point Light / Spot Light**, **Light Probe**,
  **Camera**, **Audio**, **Effects** (decal, particles), **Environment**,
  **Fog**, **Empty**;
- **Physics**: colliders, Rigid Body, Character Controller, and a ready-made
  **Character** prefab;
- the project's prefabs;
- **Component >** *any type*: an empty entity with that one component, named
  after it. Your game module's component types appear here too.

Start typing to search (`sun` finds **Light > Sun**), then use the arrow keys
and `Enter`. New objects appear at the **3D cursor** (place it with
`Shift`+right click on a surface).

> 📷 **Screenshot needed — `GUIDE-03-ADDMENU`:** *The Shift+A Add menu open over
> the viewport with "li" typed, showing the filtered Light entries.*

Right after adding or duplicating, the **Adjust Last Operation** panel at the
bottom-left of the viewport lets you change the new object's location, size,
or offset; the operation re-runs with the new values.

> 📷 **Screenshot needed — `GUIDE-03-ADJUSTLAST`:** *The Adjust Last Operation
> panel after adding a cube, showing its Location and Size fields.*

You can also add from the Hierarchy panel's **Create Entity** button.

## Moving things

| Method | How |
| --- | --- |
| Gizmo | Pick Move, Rotate, or Scale on the `T` tool shelf, then drag the gizmo handles. |
| Modal keys | `G` move, `R` rotate, `S` scale. Then `X`/`Y`/`Z` to constrain to an axis, `Shift+Z` to lock one axis, type a number for an exact amount, click or `Enter` to confirm, right click or `Esc` to cancel. |
| Exact values | Type them in the Inspector's Transform section or the sidebar's **Item** tab. |
| Reset | `Alt+G`, `Alt+R`, `Alt+S` clear location, rotation, scale. |

**Pivot point** (`.` or **Gizmos** menu): Median Point, Active Element,
Individual Origins, or 3D Cursor.

**Snapping** (`Shift+Tab` toggles; holding `Ctrl` while moving inverts it).
**Snap To** chooses:

- **Increment**: whole steps of the snap values; tick **Absolute Grid Snap**
  to land on the world grid;
- **Surface**: the selection's centre lands on the surface under the mouse;
- **Vertex**: it lands on the nearest mesh vertex.

Axis constraints still apply while snapping. Each viewport remembers its own
snapping choices.

> 📷 **Screenshot needed — `GUIDE-03-SNAPPING`:** *The Gizmos menu open with
> Snapping expanded, showing Snap To (Increment, Surface, Vertex) and the
> increment fields.*

**The 3D cursor** (the red-and-white ring) marks where new objects appear and
can be the pivot point. `Shift`+right click places it on the surface under the
mouse. The viewport header's **View > Snap** moves the cursor to the selection or the world origin,
or the selection to the cursor. It is saved with the scene (in
`<scene>.editor.json`), is not affected by undo, and can be hidden with
**Overlays > 3D Cursor**. Lights, cameras, and emitters have no mesh to click:
select them by their overlay icons.

**Array and scatter.** **Object > Array** copies the selection in a row;
**Object > Scatter** copies it across nearby surfaces. Change the count or
seed afterwards in Adjust Last Operation.

## Parenting and the hierarchy

Drag rows in the Hierarchy onto another object to parent them. A child's
transform is relative to its parent, so moving the parent carries the
children. `]` and `[` select children and parent.

The Hierarchy's dropdown switches what it lists:

| Mode | Shows |
| --- | --- |
| **View Layer** | Objects grouped by collection. |
| **Collections** | The collection tree only. |
| **Prefabs** | Prefab instances under their prefab. |
| **Orphan Data** | Project assets the scene does not use, and empty collections. |

`Ctrl`+click adds to the selection; `Shift`+click selects a range. The eye,
arrow, and camera icons on each row hide an object in the viewport, make it
unselectable, or stop it rendering. Search ignores case and finds matches
inside collapsed parents.

> 📷 **Screenshot needed — `GUIDE-03-HIERARCHY`:** *The Hierarchy in View Layer
> mode with two collections expanded, a parent/child relationship visible, and
> the eye/arrow/camera icons on the right of the rows.*

## Components and the Inspector

Select an object to see its components in the **Inspector**. Each component is
a collapsible section.

- **Add Component** opens a searchable menu grouped by category.
- Right click a section header (or its **...** button) for **Remove**,
  **Reset to Default**, **Copy Values** / **Paste Values** (JSON on the
  clipboard), **Copy to Selected**, and **Move Up/Down/to Top/to Bottom**.
- Hover a field for its tooltip; drag a number field to scrub it.

**Multi-editing.** With several objects selected, the Inspector lists the
components they share. A value that differs shows `—`; editing it sets that
field on every selected object as one undo step. Components only the active
object has appear under **Active Entity Only**; hold `Alt` while editing one
to apply it to every selected object that has it.

> 📷 **Screenshot needed — `GUIDE-03-INSPECTOR`:** *The Inspector with three
> objects selected, showing a shared Point Light section where the colour
> differs (`—`) and the Active Entity Only heading below.*

The **Properties** panel shows the same data in Blender-style tabs: **Object**
(name and transform), one tab per component category the object has, **World**
(sun, environment, fog, irradiance volumes, without selecting them), **Render**
(project render settings), and **Scene** (document summary).

> 📷 **Screenshot needed — `GUIDE-03-PROPERTIES`:** *The Properties panel on the
> World tab, with the vertical tab strip on its left edge visible.*

### Built-in component types

| Category | Components (scene key) |
| --- | --- |
| Rendering | Model (`model`), Mesh Renderer (`mesh_renderer`), Material Override (`material_override`), Camera (`camera`), Decal (`decal`), Visibility (`visibility`) |
| Lights | Directional Light (`directional_light`), Point Light (`point_light`), Spot Light (`spot_light`), Environment Light (`environment_light`), Reflection Probe (`reflection_probe`), Irradiance Volume (`irradiance_volume`) |
| Environment | Volumetric Fog (`volumetric_fog`) |
| Effects | Particle Emitter (`particle_emitter`) |
| Physics | Collider (`collider`), Rigid Body (`rigid_body`), Character Controller (`character_controller`) |
| Gameplay | First Person Camera (`first_person_camera`), Tags (`tags`) |
| Animation | Animation, Animator, Skin, Morph Weights |
| Audio | Audio Listener (`audio_listener`), Audio Emitter (`audio_emitter`) |
| Navigation | Navigation Agent (`nav_agent`) |
| Prefabs / Collections | Managed by the prefab and collection commands; you rarely add these by hand. |

Your game module adds its own categories (the templates use **Gameplay** and
**Behaviours**).

## Models and the Asset Browser

The Asset Browser shows the project's `assets/` and `scenes/` folders, and its
`src/` folder when the project has a module.

> 📷 **Screenshot needed — `GUIDE-03-ASSETBROWSER`:** *The Asset Browser in grid
> view on a folder with models, textures (with thumbnails), a prefab, and a
> .zmat material; the folder tree and favourites on the left; the search box
> and type filter.*

- **Import**: copy `.gltf`/`.glb` files, images, and audio into the folder, or
  drop files from your file manager onto the browser. Every asset gets a GUID
  in a `.meta` sidecar file next to it; keep the sidecars in version control.
- **Place a model**: drag it into the viewport. It lands on the surface under
  the mouse; tick **Align to Normal** in Adjust Last Operation to stand it on
  the slope. Dropping a model on an object's Inspector replaces its model.
- **Rename and move** in the browser: references in scenes and prefabs follow
  automatically, and changes made outside the editor are picked up too.
- **Double-click**: scenes open, prefabs open in a prefab window, source files
  open in your IDE.
- **Import problems** appear in the Diagnostics panel with a suggested next
  action.

Supported formats: glTF 2.0 (`.gltf`, `.glb`) with metallic-roughness PBR
materials, skins, and animation; PNG/JPEG/KTX2 textures; WAV and Ogg audio;
`.cube` colour grades; `.zmat` materials; JSON data tables. Details:
[Assets](../engine/assets.md).

## Materials

A model's materials come from its glTF file. To change how one object looks
without editing the model:

- **Material Override** component: tint the base colour, emission, roughness,
  and metallic factors per object, or swap in a `.zmat` material asset.
- **New Material** in the Asset Browser's folder menu writes a
  `New Material.zmat` you can edit in the browser and drag onto a material
  field.

> 📷 **Screenshot needed — `GUIDE-03-MATERIAL`:** *Two copies of the same model
> side by side, one with a red Material Override tint, with the Material
> Override section visible in the Inspector.*

## Lighting a scene

A typical outdoor scene needs three things, all in the Add menu and on the
Properties **World** tab:

1. a **Sun** (Directional Light): direction, colour, intensity, shadows;
2. an **Environment** (Environment Light): the sky and image-based lighting,
   from an HDR image or a procedural sky;
3. optional **Fog** (Volumetric Fog).

Indoors, add **Point** and **Spot** lights, **Reflection Probes** for local
reflections, and an **Irradiance Volume** for baked bounce light. Use the
**Lighting** workspace to see the World and Render settings side by side, and
**Shading > Rendered** to see the final look.

> 📷 **Screenshot needed — `GUIDE-03-LIGHTING`:** *The Lighting workspace on the
> showcase project: World tab with sun and environment, Render tab, and the
> Graphics panel's GPU timings.*

Render settings (antialiasing, bloom, exposure, SSR, AO, colour grading, ...)
belong to the project and are edited on the **Render** tab or in the Graphics
panel. See [Graphics post-processing](../engine/graphics-post.md).

## Physics objects

To make an object solid, give it a **Collider** and a **Rigid Body**:

| Rigid body motion type | Use for |
| --- | --- |
| **Static** | Walls, floors, level geometry. Never moves. |
| **Kinematic** | Platforms and doors moved by code; pushes dynamic bodies but is not pushed. |
| **Dynamic** | Crates and props simulated by physics. |

Collider shapes are Box, Sphere, Capsule, Triangle Mesh (exact, static only),
and Convex Hull (matches a mesh, works on moving bodies). Tick **Sensor** on a
rigid body to make a trigger volume that reports entries and exits instead of
colliding. The collider's offset moves the shape away from the entity's
origin; the viewport draws colliders (faintly until selected) and lets you
drag their handles.

A player uses a **Character Controller** instead of a rigid body. Details:
[Physics](../engine/physics.md).

> 📷 **Screenshot needed — `GUIDE-03-COLLIDERS`:** *A scene with the Collider
> overlay set to All, a selected box collider showing its edit handles, and a
> sensor volume drawn in a different colour.*

## Prefabs: reusable objects

A **prefab** is a saved group of objects (a `.prefab` file) you can place many
times. Editing the prefab updates every placed copy (**instance**), while each
instance can still **override** individual fields.

| Action | How |
| --- | --- |
| Create | Select objects, **Object > Prefab > Create from Selection**. |
| Place | Drag the `.prefab` from the Asset Browser into the viewport, or use the Add menu. |
| Edit | Double-click the `.prefab`, or **Object > Prefab > Edit Prefab** on an instance. It opens in its own **prefab window**. |
| Override | Change a field on an instance. Overridden fields are marked in the Inspector. |
| Apply | **Object > Prefab > Apply** writes the instance's overrides into the prefab. |
| Revert | **Object > Prefab > Revert** drops the overrides. |
| Unpack | **Object > Prefab > Unpack** turns an instance into plain objects. |

**Prefab windows.** Edit Prefab opens the prefab in a floating window with its
own Hierarchy, Viewport, Inspector, **Save**, and **Close**. The level stays
open, editable, and even playable underneath, and you can have several prefab
windows open at once. Undo, `Ctrl+S`, and the main menu act on whichever
window last had focus. Saving refreshes every instance in the level as an
undoable step. The window's viewport uses preview lighting rather than the
prefab's own sun and environment.

> 📷 **Screenshot needed — `GUIDE-03-PREFABWINDOW`:** *A prefab window floating
> over the level, showing its own Hierarchy, Viewport (with a selected child
> and gizmo), Inspector, and the Save/Close buttons; the level visible behind.*

Prefabs nest: a room prefab can contain door prefabs. Deep dive:
[Prefab windows](../editor/prefab-windows.md), [Scene › prefabs](../engine/scene.md).

## Collections: organising a level

**Collections** group objects the way Blender does. They are separate from
parenting, and an object can belong to several.

- `F3` → **New Collection** makes one holding the selection;
  **Link to Collection** / **Remove from Collection** change membership;
  **Delete Collection** moves its contents to its parent.
- Each collection can be hidden in the viewport, made unselectable, or
  excluded from rendering.
- **Add Collection Instance** places an empty that draws a collection's
  objects at its own transform: a cheap way to repeat a set piece.

Collections are saved with the scene, and every change undoes.

## Hiding versus excluding

| You want | Do this |
| --- | --- |
| Get something out of your way while editing | `H` hides it (`Shift+H` hides everything else, `Alt+H` reveals all). It still appears in Play and in the game. |
| Leave something out of the game | Turn off **Renderable** on its **Visibility** component or its collection. |

## Tags

Add a **Tags** component and pick from the project's tag list (edited in
**Edit > Project Settings... > Tags**). Game code finds tagged objects with
`findEntitiesWithTag(scene, "enemy")`. A tag the project does not declare is
flagged, because code may have misspelled it.

## Undo

Every edit — Inspector fields, gizmo moves, prefab and collection commands,
Apply to Edit Scene — is one undo step (`Ctrl+Z`; redo with `Ctrl+Shift+Z` or
`Ctrl+Y`). Undo history is per document: the level and each prefab window have
their own.

[Guide contents](README.md) · Previous: [The editor at a glance](02-editor-tour.md) · Next: [Playing and debugging](04-play-and-debug.md)
