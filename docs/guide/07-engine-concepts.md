# 7. How the engine works

[Guide contents](README.md) · Previous: [Gameplay services](06-gameplay-services.md) · Next: [Shipping a game](08-shipping.md)

You can make games without reading this chapter. Read it when you want to
know *why* the engine behaves as it does, or before changing the engine
itself. It is a guided summary; [Architecture](../ARCHITECTURE.md) gives the
overview and the subsystem pages hold every detail. The mathematics behind the main mechanisms
is spelled out, because it explains behaviour you will observe.

## The layers

```text
 your game module  ·  the editor  ·  the demo           (policy: what the game is)
              │            │            │
              ▼            ▼            ▼
 ┌──────────────────── zenith_engine (shared library) ─────────────────────┐
 │ application lifecycle · module loading · GameServices                     │
 │ scene (ECS) · assets · input · physics · audio · navigation · animation   │
 │ graphics (Vulkan) · UI · platform                                          │ (mechanism)
 └───────────────────────────────────────────────────────────────────────────┘
              │
              ▼
 SDL 3 · Vulkan 1.4 + VMA · Jolt · EnTT · GLM · cgltf · Recast/Detour · ...
```

Dependencies point **down** only. The engine never knows which game is
running; the editor and games depend on the engine, never the reverse. Inside
the engine, subsystems talk through small domain types and handles: physics
does not know about rendering, assets do not own GPU memory, and input does
not know what "jump" means. This is what lets one engine serve many games.

| Program | Composition root | Runs |
| --- | --- | --- |
| `zenith` (game runner) | `src/main.cpp` | `ProjectApplication` (any project + its module) or the built-in demo. |
| `zenith_editor` | `src/editor/main.cpp` | `EditorApplication` / `EditorSession`. |
| Your module | `GameModule.cpp` | Registers components, systems, behaviours into either host. |

## One frame

Rendering runs at whatever rate the display allows; simulation runs at a
**fixed** rate (60 Hz by default). Each frame:

```text
1. Poll SDL events → InputState; evaluate input actions
2. GameServices::beginFrame          (last frame's events become readable)
3. Update systems + behaviours       (game clock dt)
   timers due on the game clock
4. accumulator += min(frame time, 100 ms) × time scale
   while accumulator ≥ h and steps < max:      (h = 1/60 s, max = 4)
       FixedUpdate systems + behaviours (trigger/collision hooks first)
       physics step (Jolt)
       accumulator -= h
5. Interpolate dynamic bodies between the last two physics states
6. LateUpdate systems + behaviours   (cameras follow bodies here)
7. Animation, world transforms, audio sync
8. Render the HDR scene → post-processing → tone map
9. Ui systems (HUD, menus) → present
10. Apply deferred scene switches / quit
```

Entity commands (spawns, destroys) flush after each phase, so no system ever
sees the scene change under it.

### Why fixed steps, and what interpolation does

Physics integrators are only stable and deterministic with a constant step
*h*. Rendering at, say, 144 Hz while simulating at 60 Hz means most frames
fall *between* two physics states. With the leftover accumulator *a*
(0 ≤ *a* < *h*), the interpolation factor is

$$\alpha = \frac{a}{h} \in [0, 1),$$

and each dynamic body is drawn at

$$p_{\text{draw}} = (1-\alpha)\,p_{n-1} + \alpha\,p_n,\qquad
q_{\text{draw}} = \operatorname{slerp}(q_{n-1}, q_n, \alpha).$$

So what you see lags the simulation by up to one step (≈16.7 ms), in exchange
for perfectly smooth motion at any frame rate. The frame-time clamp (100 ms)
and the step cap (4) stop a "spiral of death" where a slow frame causes more
steps, which cause a slower frame.

Practical consequences:

- Put forces, velocities, and character input in **FixedUpdate**; in Update
  they would apply once per *frame* rather than once per *step*.
- Put cameras in **LateUpdate**, after interpolation, or they will jitter
  against the bodies they follow.
- A short key press between two physics steps is latched until a step
  consumes it, so jumps are never lost.

### Time scale and pause

The game clock multiplies the editor's and the game's time scales and pauses
(either pause wins). With scale *s*, simulated time advances by *s·Δt*: physics,
FixedUpdate, timers, animation, and scene audio all slow together. A frame
step while paused advances exactly one *h*.

## Scenes and the ECS

A `Scene` wraps an **EnTT** registry. An *entity* is just an id; *components*
are plain structs stored in tightly packed arrays per type; *systems* iterate
the entities that have a given set of components. This keeps data
cache-friendly and makes features composable: an entity becomes physical by
gaining a collider and a rigid body, renderable by gaining a mesh renderer.

Every entity always has `NameComponent`, `TransformComponent`, and
`RelationshipComponent` (parent + children), so hierarchy code never needs
optional branches.

Two kinds of identity:

| Type | What it is | Use for |
| --- | --- | --- |
| `Entity` | The EnTT handle. Fast, but can be recycled after destruction and changes when an entity is restored by undo. | Within one function or frame. |
| `SceneEntityId` | A stable 64-bit id saved in the scene file. | Anything kept across frames, references between entities, saved data. |

**Reflection** (`Fields<T>`) describes component fields explicitly. From one
description the engine derives JSON serialization, Inspector widgets,
undo/redo diffs, multi-edit, prefab overrides, and entity-reference
remapping. There is no code generation.

## Coordinates and transforms

| Convention | Value |
| --- | --- |
| Handedness | Right-handed |
| Up | +Y |
| Forward | −Z (a camera looks down its local −Z) |
| Right | +X |
| Units | metres, seconds, radians in code (the Inspector shows degrees) |
| Matrices | GLM, column-major, column vectors (`M * v`) |
| Rotations | unit quaternions (`glm::quat`) |

Each entity stores a **local** transform relative to its parent. World
transforms compose down the hierarchy:

$$W_{\text{entity}} = W_{\text{parent}} \cdot L_{\text{entity}},\qquad
W_{\text{root}} = L_{\text{root}}.$$

A local transform is built from translation **t**, rotation *q*, and scale
**s** as *L = T(**t**) · R(q) · S(**s**)*, so a point is scaled, then
rotated, then translated. Going the other way (`decomposeTrs`), the
translation is the last column, each scale factor is the length of a basis
column, and the rotation is the Gram–Schmidt orthonormalised basis (a
mirrored basis negates X). Shear cannot survive decomposition, which is why
`setRotation` and `setScale` drop it.

`TransformComponent::world` is a **cache**. `updateWorldTransforms` refreshes
only branches marked dirty, so its cost is proportional to what changed, not
to the scene size. That is why setters mark dirty and direct writes must call
`markTransformDirty`. `Scene::worldTransform(e)` instead multiplies up the
parent chain on demand (one matrix product per ancestor), so it is correct
even right after a setter.

Physics decomposes world transforms into position, rotation, and absolute
scale; scale is baked into the collision shape, and the simulation writes back
position and rotation only, so visual scale is preserved.

## Game modules and hot reload

A module is a shared library with a C-linkage entry point. Loading it:

1. **Shadow copy.** The editor copies `my_game.zmodule` (and its debug
   symbols) to a unique file and loads the copy, so your compiler can
   overwrite the original while the editor runs.
2. **Compatibility check.** The module reports the engine module-API version
   and a toolchain/runtime build hash; a mismatch is refused with a message.
3. **Registration** into a *copy* of the built-in component registry, so a
   module can never corrupt the engine's types.
4. **Reload.** Before unloading, every scene's module components are
   serialised through their reflected fields, the services and behaviours are
   torn down, and the library unloaded. After loading the new build, the
   components are deserialised again. A module that fails to load leaves its
   components as preserved JSON.

That is why state must live in components or session resources, never module
statics, and why `Fields<T>` is the contract that makes hot reload work.

## Assets

```text
path ─► AssetManager::loadModel ─► parse (cgltf) ─► validate limits ─► decode images
     ─► optimise meshes, build LODs (meshoptimizer) ─► commit to generational slots
     ─► typed handle (MeshHandle, MaterialHandle, TextureHandle...) in components
     ─► renderer uploads to the GPU lazily, on first visible use
```

- **Handles are generational**: a slot index plus a generation counter. A
  stale handle to a reloaded or freed asset is detected instead of reading
  the wrong data.
- **Leases** keep an asset alive while a scene, a renderer frame in flight,
  or an audio voice uses it; collection reclaims unleased assets within
  budgets.
- **GUIDs** in `.meta` sidecars give every project asset a permanent identity,
  so scenes survive renames and moves.
- **Cooked textures**: `zenith_asset_build` compresses textures offline to
  BC7 (colour, data) and BC5 (normals) KTX2 files in a derived-data cache
  keyed by source bytes, settings, tool version, and platform. Missing cooked
  data falls back to RGBA8.

Deep dive: [Assets](../engine/assets.md).

## Rendering

Zenith renders with **Vulkan 1.4** using dynamic rendering and
synchronization2, three frames in flight, bindless materials, and
optionally GPU-driven culling (compute shaders build the draw lists, with a
two-phase occlusion test against a depth pyramid).

### The shading model

Surfaces use the glTF **metallic-roughness** PBR model with a Cook–Torrance
microfacet BRDF (`shaders/mesh/brdf.glsl`, part of `mesh.frag`). For light direction **l**, view
direction **v**, normal **n**, and half vector **h** = normalize(**l** + **v**):

$$f(\mathbf{l},\mathbf{v}) = \underbrace{(1-F)(1-m)\frac{\mathbf{c}}{\pi}}_{\text{diffuse}}
+ \underbrace{\frac{D\,G\,F}{4\,(\mathbf{n}\cdot\mathbf{v})(\mathbf{n}\cdot\mathbf{l})}}_{\text{specular}}$$

with base colour **c**, metallic *m*, perceptual roughness *r*, and α = *r*²:

| Term | Formula | Meaning |
| --- | --- | --- |
| *D* (GGX / Trowbridge–Reitz) | $\dfrac{\alpha^2}{\pi\,\big((\mathbf{n}\cdot\mathbf{h})^2(\alpha^2-1)+1\big)^2}$ | How many microfacets face **h**. |
| *G* (Smith, Schlick-GGX) | $G_1(x)=\dfrac{x}{x(1-k)+k}$, $k=\dfrac{(r+1)^2}{8}$, $G=G_1(\mathbf{n}\cdot\mathbf{v})\,G_1(\mathbf{n}\cdot\mathbf{l})$ | Microfacets shadowing each other. |
| *F* (Schlick) | $F_0 + (1-F_0)(1-\mathbf{h}\cdot\mathbf{v})^5$, $F_0 = \operatorname{mix}\!\left(\left(\tfrac{\eta-1}{\eta+1}\right)^2, \mathbf{c}, m\right)$ | Reflectance rising at grazing angles. With the default index of refraction η = 1.5 the dielectric $F_0$ is 0.04 (`KHR_materials_ior`/`specular` can change it). |

Outgoing radiance from one light is *f* · *L*ᵢ · (**n**·**l**). Image-based
lighting uses a GGX-prefiltered specular cube (split-sum with a BRDF lookup
table) and spherical-harmonic irradiance for diffuse.

### The pipeline in one picture

```text
environment precompute (when the sky changes)
→ shadow cascades (sun) + local light shadows
→ GPU culling → light & decal clustering (16×9×24 froxels) → volumetric fog → particles
→ depth/normal prepass → depth pyramid → ambient occlusion (GTAO)
→ opaque shading (HDR) → sky → screen-space reflections → transparent surfaces → particles
→ TAA / FSR upscaling → depth of field → motion blur
→ exposure (luminance histogram) + bloom → tone map (AgX by default) → colour grade
→ game UI → present
```

**Clustered lighting.** The view frustum is divided into 16 × 9 screen tiles
and 24 depth slices; each cluster stores the lights touching it, so a pixel
loops over only nearby lights. Slices are spaced **logarithmically** in view
depth *z*, which keeps clusters roughly cube-shaped:

$$\text{slice}(z) = \left\lfloor \log z \cdot \frac{N}{\log(z_f / z_n)} - \frac{N\log z_n}{\log(z_f / z_n)} \right\rfloor = \left\lfloor N\,\frac{\log(z/z_n)}{\log(z_f/z_n)} \right\rfloor,\quad N = 24,$$

where $z_n$ is the near depth and $z_f = \min(z_{\text{far}}, 500\,\text{m})$
(`ClusterGridConfiguration::maximumSliceDepth`); the last slice extends to the
far plane (`Lighting.cpp`, mirrored by `fragmentCluster` in `mesh.frag`). Each slice
spans the same *ratio* of depths, $(z_f/z_n)^{1/N}$.

**Cascaded shadows.** The sun's shadow map is split into cascades covering
progressively farther depth ranges, each an orthographic projection fitted to
its slice of the frustum and snapped to texels so shadows do not shimmer as
the camera moves. Filtering is percentage-closer (PCF).

Each pass is timed on the GPU; the **Graphics** and **Render Statistics**
panels show the numbers. Deep dives: [Graphics](../engine/graphics.md),
[Scene passes](../engine/graphics-passes.md),
[Post-processing](../engine/graphics-post.md),
[Shaders](../engine/shaders.md).

> 📷 **Screenshot needed — `GUIDE-07-RENDERSTATS`:** *The Render Statistics
> panel and Graphics panel GPU timing table on the showcase project, showing
> per-pass milliseconds.*

## Physics

Zenith wraps **Jolt Physics** behind ECS components. Each step, the physics
world discovers entities with a transform, collider, and rigid body; creates,
rebuilds, or removes Jolt bodies to match; submits kinematic poses; steps; and
copies dynamic poses back. Character controllers use Jolt's
`CharacterVirtual`: each step blends velocity toward `desiredVelocity` (at
`acceleration`, scaled by `airControl` in the air), rejects slopes steeper than
the limit, walks up steps, sticks to the ground going down, and rides moving
platforms.

Collision filtering is a 32-layer table with a symmetric collision matrix
*C* (*C*ᵢⱼ = *C*ⱼᵢ): two bodies on layers *i* and *j* collide iff *C*ᵢⱼ. A
query's `layerMask` is a bit set where bit *i* selects layer *i*.

Deep dive: [Physics](../engine/physics.md).

## Determinism

Several systems are designed to give the same results every run:

- system and behaviour order (phase → order → registration → entity id);
- fixed physics steps;
- `RandomStream` is PCG32 (64-bit LCG state *x*ₙ₊₁ = *a·x*ₙ + *c* mod 2⁶⁴
  with an XSH-RR output permutation), with its own range/float/shuffle helpers
  instead of `<random>`'s distributions, which differ between standard
  libraries. Named streams are seeded with SplitMix64 over the session seed
  and the name's FNV-1a hash;
- one-frame-delayed events, so publish/read order between systems does not
  matter.

This is what makes headless tests of gameplay reliable.

## Error handling

Recoverable failures return results with descriptive messages (asset loads,
scene loads, module loads, registration). Failed loads keep the previous good
state: a failed scene switch keeps the running scene, a failed asset reload
keeps the old generation, a failed module build keeps the old code running.
Game code that throws is contained (logged, disabled, Play stopped). Crashes
go to a crash handler that saves your work first.

[Guide contents](README.md) · Previous: [Gameplay services](06-gameplay-services.md) · Next: [Shipping a game](08-shipping.md)
