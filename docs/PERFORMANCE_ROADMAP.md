# Zenith performance and capability roadmap

This document contains the objectives remaining after completion of the
performance-baseline work. The goal is not merely a high peak frame rate; it is
stable frame delivery, scalable rendering and physics, predictable memory use,
and quality features that remain measurable and optional.

## Current status

### Step 1: reproducible measurement foundation — complete

Zenith now provides full-runtime and render-only benchmark modes, a standalone
render-packet benchmark, and sleeping/active rigid-body benchmarks. Reports
contain build and hardware metadata, CPU and GPU scope distributions, retired
GPU submission identities, memory counters, and budget misses. Percentile,
timestamp-wrap, budget, and sample-de-duplication behavior has deterministic
test coverage.

The remaining operational task is to archive Release baselines for the
reference machines. A Debug or smoke run proves that instrumentation works; it
does not establish a production performance result.

## Measurement contract for every remaining objective

Before changing performance-sensitive code:

1. Choose one primary metric, statistic, unit, direction, and target. Prefer
   p95 for interactive stability and also inspect median, p99, maximum, and
   budget misses.
2. Capture an untouched Release baseline with the same scene, workload size,
   random seed, renderer path, resolution, present mode, driver, hardware,
   power state, and warmup.
3. Limit each optimization experiment to a named subsystem and a fixed number
   of attempts. Do not combine unrelated changes into one candidate.
4. Run functional tests and the relevant visual or physics correctness checks
   before comparing timing results.
5. Reject results with changed metadata, missing samples, validation errors,
   visual regressions, simulation divergence, or material secondary-metric
   regressions.
6. Treat a faster candidate as correlation, not proof of cause. Retain the
   change only when the result repeats outside observed run-to-run noise.

Recommended reference budgets are 16.67 ms for 60 Hz and 8.33 ms for 120 Hz.
Scene-specific CPU, GPU, streaming, and memory budgets must divide those totals
explicitly instead of each subsystem independently claiming the whole frame.

## Required baseline matrix

Before Step 2 begins, record at least five Release runs per scenario and keep
the raw JSON reports outside the source tree or in an intentionally managed
telemetry store.

| Scenario | Workload | Primary evidence |
| --- | --- | --- |
| Render submission | 2,000, 10,000, and 50,000 repeated instances with static and moving variants | CPU renderer and packet-build p95, draw calls |
| GPU visibility | Mostly visible, mostly occluded, and camera-sweep variants | GPU frame p95, visible count, false-pop capture |
| Lighting | Indoor/outdoor scenes at several local-light and shadow-caster counts | Opaque/shadow GPU p95, discarded assignments, reference captures |
| Animation | Fixed populations of skinned and morph-animated characters | Animation CPU p95, upload bytes, GPU opaque p95 |
| Physics sleeping | 256, 1,000, and supported-maximum sleeping bodies | Whole physics-step and sync p95, active/sleeping counts |
| Physics active | Falling stacks, contacts, and sustained impulses | Jolt/sync p95, contact count, deterministic end state |
| Streaming | Repeated additive load/unload while moving a fixed camera path | Frame p99, upload bytes, memory high-water mark, hitch count |
| Full runtime | Representative authored gameplay path with a fixed input script | CPU/GPU frame p95, budget misses, correctness hash |

Each scripted scenario should own a stable version and seed. If its content or
script changes, it starts a new baseline series.

## Step 2: eliminate avoidable CPU frame work

The first optimization target is work performed every frame even when relevant
state has not changed.

### Implementation

- Add subsystem revision counters for transform, renderable, light, camera,
  skin, and material changes. Build immutable frame snapshots only when their
  input revision changes.
- Split render-packet preparation into persistent object records and compact
  per-frame visibility results. A transform change should update one object
  record rather than rebuild or sort the entire scene.
- Replace physics' full ECS/body reconciliation scan with explicit scene
  lifecycle notifications queued as create, update, and destroy operations.
  Preserve a periodic debug audit that compares the queue-driven state with
  ECS state.
- Read dynamic transforms back only for active bodies. Sleeping bodies retain
  their last transform until Jolt reports an activation or removal.
- Reuse vectors, command lists, temporary sort keys, UI conversion buffers,
  and contact-event storage. Add allocation counters to prove that steady-state
  frames allocate nothing on the main thread.
- Cache static camera-independent bounds and recompute world bounds only when
  the corresponding transform or deformation revision changes.

### Measurement and completion gate

- Compare static, 1%-moving, and 100%-moving versions of the render and physics
  workloads.
- Track main-thread frame time, render-packet time, physics sync time, scanned
  entities, updated records, and allocations per frame.
- Complete this step when static-frame work scales with changed objects rather
  than total objects, steady-state allocations are zero, and no measured
  scenario regresses beyond its recorded noise band.

## Step 3: build an engine-wide task graph

Zenith has a bounded job system, but high-cost frame phases must be expressed as
dependencies before they can use all CPU cores safely.

### Implementation

- Introduce a frame-local task graph owned by the engine orchestration layer.
  Nodes declare read/write access to scene snapshots and produce immutable
  outputs; subsystems must not discover jobs through global state.
- Start with independent animation evaluation, world-bound updates, light-grid
  construction, render visibility, audio spatialization, and non-conflicting
  asset work.
- Partition work into stable index ranges with a minimum grain size derived
  from benchmark results. Use thread-local scratch buffers and merge results in
  deterministic entity or object order.
- Keep ECS mutation and Vulkan command submission on their owning threads.
  Worker jobs consume snapshots or narrowly synchronized arrays rather than
  mutating the registry.
- Add cancellation and frame-generation checks so scene transitions cannot
  publish stale results. Expose queue depth, worker utilization, stolen jobs,
  and main-thread assist time in benchmark output.

### Measurement and completion gate

- Compare 1, 2, 4, 8, and automatic worker counts for animation, culling, and
  mixed full-runtime workloads.
- Measure total phase latency as well as aggregate worker time; higher worker
  utilization is not success if p95 frame time worsens.
- Complete this step when the chosen workloads scale on the reference CPUs,
  single-thread deterministic mode remains correct, and transition/cancellation
  stress tests pass under sanitizers where available.

## Step 4: persistent GPU scene and GPU-driven visibility

The renderer should submit large scenes in work proportional to visible
batches, not total entities.

### Implementation

- Create renderer-owned, generation-checked GPU object, material, mesh, and
  instance tables. Engine handles remain opaque; Vulkan addresses and
  descriptors remain private to graphics.
- Upload table deltas through the existing staging path and retain compatible
  per-frame resources until their fences retire.
- Build a depth pyramid from the previous frame. Run compute frustum and Hi-Z
  occlusion culling into compact visible-instance and indirect-command buffers.
- Add mesh and material binning so compatible instances become one indirect
  draw. Use indirect-count commands only when capabilities allow them.
- Introduce descriptor indexing only behind capability checks. Maintain the
  existing instanced path as a correctness fallback for unsupported devices.
- Detect camera cuts, newly spawned objects, and invalid depth history; bypass
  occlusion for one frame to prevent false disappearance.
- Keep CPU reference culling available in tests. Periodically compare GPU
  visibility results against conservative CPU results in diagnostic builds.

Milestone 25 replaced the per-level depth-pyramid dispatches with a single-pass
downsampler and made the post-prepass rebuild on the GPU-driven path
conditional on the late phase drawing anything, so the pyramid is built once
per frame in the steady state. A full build fell from about 0.067 ms to 0.047
ms at 1280x720 (RTX 2070 SUPER); GPU-driven showcase frames fell from 1.551 ms
to 1.466 ms median. See `RENDERER_VALIDATION.md` for the full A/B table.

### Measurement and completion gate

- Benchmark visible-heavy, occluded-heavy, camera-cut, and rapidly moving
  scenes at multiple instance counts.
- Primary metrics are CPU renderer p95, GPU culling p95, total GPU frame p95,
  draw count, and uploaded bytes. Visual captures must show no popping.
- Select the GPU-driven path by default only where it produces a repeated,
  material improvement on supported hardware without a GPU-frame regression.

## Step 5: render graph, transient memory, and asynchronous transfers

A declarative render graph is justified when it replaces measurable barrier,
allocation, and scheduling costs rather than merely renaming passes.

### Implementation

- Define pass declarations containing image/buffer reads, writes, formats,
  extents, queue requirements, and lifetime. Compile declarations into an
  execution order, synchronization plan, and transient-resource allocation.
- Import persistent swapchain, history, shadow, probe, and asset resources;
  allocate only truly transient attachments from graph-managed pools.
- Derive Vulkan synchronization2 barriers from declared hazards and validate
  them against a conservative debug mode.
- Alias non-overlapping transient images only when format, usage, alignment,
  and device requirements are compatible. Report requested, committed, aliased,
  and peak bytes.
- Move asset transfers to a dedicated transfer queue when available. Signal a
  timeline semaphore and make only consuming passes wait for the required
  value; never introduce a device-wide idle.
- Evaluate asynchronous compute for light binning, depth-pyramid generation,
  bloom, and later effects. Enable overlap only when timestamps demonstrate a
  net gain on that queue topology.

### Asynchronous compute evaluation (2026-10-02)

Evaluated for light clustering, the depth pyramid, and bloom on an RTX 2070
SUPER (driver 576.80; queue families: 16 graphics+compute, 2 transfer, 8
compute-only). Release, 1280x720, 400 measured frames after 120 warmup, median
GPU milliseconds from the per-pass timestamps:

| Scene | GPU frame | Light clusters | Depth pyramid | Bloom chain | Exposure |
| --- | ---: | ---: | ---: | ---: | ---: |
| Showcase | 1.48 | 0.029 | 0.068 | 0.077 | 0.039 |
| Demo | 0.26 | 0.001 | 0.071 | 0.075 | 0.031 |
| 4,000 objects | 0.77 (clock-dependent, 0.9-5.8) | 0.028 | 0.067 | 0.081 | — |

Not implemented; the measured ceiling does not justify the mechanism:

- **Upper bound.** Even perfect overlap hides at most the three passes'
  combined ~0.17 ms (~11% of the showcase frame, ~0.15 ms of the
  4,000-object frame). Each pass is a few short dispatches that already
  overlap neighbouring graphics work inside one command buffer; a compute
  queue only helps where the graphics queue is otherwise serialized.
- **Dependencies leave little to overlap.** The depth pyramid reads the
  prepass depth and feeds the late GPU cull, ambient occlusion, and
  screen-space reflections, so it sits on the critical path with nothing
  independent to run beside it. Bloom reads the temporally resolved HDR target
  and feeds tone mapping; overlap needs cross-frame pipelining against the
  next frame's shadow pass, which the single per-frame submission does not
  support. Light clustering (CPU-uploaded lights only) is the one candidate
  that could overlap shadows and the prepass, and it costs ~0.03 ms.
- **Cost of the mechanism.** The device has no compute queue today, the render
  graph's queue parameter only splits the asset-upload submission, and the
  cluster, pyramid, and bloom resources are exclusive to the graphics family.
  Async compute would need per-frame submission splitting, timeline
  semaphores between queues, and queue-family ownership transfers (or
  concurrent sharing, which can disable compression on render targets). Each
  extra submission and semaphore wait costs tens of microseconds on this
  driver, comparable to the clustering pass it would hide.
- **Measurement noise.** With other GPU work sharing the device, GPU frame
  times at 4,000 objects swing 0.9-5.8 ms run to run because a CPU-bound frame
  lets the GPU drop clocks; a ~0.03-0.17 ms gain cannot be demonstrated
  through that noise, so the roadmap's "enable only on a demonstrated net
  gain" gate cannot be met.

Revisit when a pass becomes long enough to matter (for example volumetric fog
or GPU-driven culling at much larger scene counts) and the graph can split a
frame into multiple submissions.

### Measurement and completion gate

- Measure GPU frame time, barrier count, transient peak bytes, queue overlap,
  upload wait time, and pipeline bubbles on at least two queue-family layouts.
- The graph is complete when output matches reference captures, validation is
  clean, resize/device-loss lifetimes are explicit, and memory or frame-time
  improvement justifies the added mechanism.

## Step 6: high-end image quality without sacrificing scalability

Quality features must be individually selectable, capability-gated, and
included in the same frame-budget system as existing passes.

### Implementation order

1. Add motion vectors, per-frame jitter, stable history resources, and camera-
   cut/disocclusion masks. These are prerequisites for temporal techniques.
2. Implement TAA with neighborhood clipping and reactive handling for
   transparency and emissive content. Add static-camera shimmer, moving-camera
   ghosting, and disocclusion reference sequences.
3. Add dynamic-resolution control driven by a smoothed GPU budget. Change
   resolution slowly, quantize extents, and invalidate temporal history when
   required. Keep native resolution as the deterministic comparison path.
4. Add scalable screen-space ambient occlusion and reflections with temporal
   denoising. Mark unavailable data explicitly and fall back to probes and IBL.
5. Improve shadows through cached static casters, per-cascade invalidation,
   atlas allocation for local lights, and measured filtering tiers.
6. Evaluate meshlet rendering and hardware ray tracing only after the
   persistent GPU scene is stable. Both remain optional paths with conventional
   raster fallbacks and separate memory budgets.

### Measurement and completion gate

- Every quality tier gets its own GPU scopes, memory counters, deterministic
  still captures, and temporal sequences.
- Track native-resolution cost separately from reconstruction cost. Do not
  describe a lower internal resolution as a renderer speedup without stating
  the quality tradeoff.
- A feature ships only when its name matches implemented behavior, unsupported
  devices select an explicit fallback, and its worst representative scene fits
  the declared GPU and memory budget.

GTAO cost (Milestone 25, 2026-10-02): full-resolution GTAO measured 0.50 ms at
1280x720 and 2.05 ms at 1920x1080 on the reference RTX 2070 SUPER. Half
resolution now traces 2 slices x 4 steps against depth-pyramid level 1 and
denoises and upsamples to the render resolution in one depth-aware pass:
0.14 ms at 720p and 0.32 ms at 1080p with visually equivalent contact
occlusion, so it is the recommended quality and the one
`ZENITH_FORCE_HIGH_QUALITY` selects. Full resolution remains available and
unchanged in cost. `RENDERER_VALIDATION.md` has the full table and the image
comparison.

## Step 7: physics scale and capability expansion

Optimize the synchronization boundary before adding more simulation features.

### Implementation

- Complete the event-driven body lifecycle and active-body transform path from
  Step 2. Batch body creation, removal, kinematic updates, and queries using
  Jolt locking/batch APIs where ownership permits.
- Separate gameplay contact needs from diagnostic contact data. Filter at the
  listener and store compact handles so irrelevant pairs do not allocate or
  cross the subsystem boundary.
- Share the engine's worker-count and affinity policy with Jolt rather than
  independently oversubscribing CPU cores. Measure both dedicated and shared
  scheduling policies.
- Add spatial and layer-specific query batches for ray, shape, and overlap
  tests. Return Zenith result types and reuse result buffers.
- Add constraint/joint support first, followed by character and vehicle
  mechanisms only when a game supplies representative workloads. Soft bodies
  remain a separate opt-in budget because their scaling and memory behavior are
  materially different.
- Define large-world coordinates or origin rebasing before claiming support for
  very large streamed worlds. Rendering, physics, audio, and particles must
  rebase at one frame boundary.

### Measurement and completion gate

- Maintain separate sleeping, active-stack, sustained-contact, query-heavy,
  constraint-heavy, and destruction-storm scenarios.
- Primary metrics are physics-step p95, sync-in/out p95, activation counts,
  contacts retained, memory, and deterministic final transforms.
- No new physics capability is complete without serialization rules, teardown
  tests, debug visualization, and a workload-specific budget.

## Step 8: streaming, memory residency, and hitch prevention

Peak speed is not useful if traversal or content arrival produces long stalls.

### Implementation

- Add explicit CPU and GPU residency states to derived assets. Track requested,
  decoding, upload-pending, resident, eviction-pending, and retired generations.
- Prioritize requests by projected visibility, distance, gameplay importance,
  and deadline. Apply bounded byte and job budgets per frame.
- Stream texture mip ranges and large mesh chunks independently. Begin with
  deterministic heuristic residency; consider GPU feedback only after the
  simpler policy is measured.
  Status: cooked (BC5/BC7) textures stream mip ranges. They become resident
  with their 64-texel tail, then `planTextureMipUploads` spends
  `RenderSettings::textureStreamingBytesPerFrame` (default 8 MiB, 0 = Off)
  per frame on the next levels, most visible first. The showcase's 43 MB of
  mip data converges in 7 frames; steady-state GPU frame time is unchanged
  (showcase 1.533 vs 1.533 ms median against the parent commit). Mesh chunks,
  screen-size-driven partial residency, and dropping distant mips under
  memory pressure remain open.
- Maintain upload rings with fence/timeline retirement and backpressure. When a
  budget is exhausted, defer low-priority work rather than blocking rendering.
- Prewarm required pipelines and cache compatible pipeline binaries. Record
  pipeline creation and first-use events as hitch diagnostics.
- Add memory-pressure responses in priority order: discard temporary caches,
  lower distant mip residency, evict unused assets, then report that the hard
  budget cannot be met.

### Measurement and completion gate

- Replay a deterministic camera path through additive scenes from cold and warm
  caches.
- Measure p99/max frame time, frames over budget, bytes decoded/uploaded,
  backlog, residency misses, pipeline creation events, and CPU/GPU peak memory.
- Complete this step when streaming respects configured budgets, cancellation
  cannot resurrect unloaded content, and traversal has no unaccounted blocking
  waits.

## Step 9: profiling, regression gates, and scalability controls

### Implementation

- Emit trace events with a common frame/submission ID for engine phases, jobs,
  Vulkan passes, uploads, asset work, and physics. Support a standard viewer
  format without making tracing mandatory in Release builds.
- Add counters for work quantities, not only time: entities scanned, objects
  updated/culled, indirect commands, pixels/internal resolution, lights per
  tile, bodies simulated, contacts, jobs, allocations, and streamed bytes.
- Define hardware tiers and quality presets as data. Each preset owns resolution
  policy, shadow, lighting, temporal, effect, physics, streaming, and memory
  budgets with validated fallbacks.
- Add scheduled performance jobs on controlled machines. CI on arbitrary
  shared runners continues to check correctness and schema, not enforce noisy
  hardware timing thresholds.
- Store baseline and candidate reports with provenance. Gate only comparable
  metrics and flag secondary regressions even when the primary target improves.

### Measurement and completion gate

- A regression report must identify the first failing metric, affected
  scenario, baseline/candidate metadata difference, and supporting correctness
  artifacts.
- A quality preset is complete only when its worst supported reference scene
  fits its CPU, GPU, and memory budgets on the declared hardware tier.

## Recommended execution order

```text
Release baseline archive
        |
        v
Step 2: eliminate idle CPU work
        |
        v
Step 3: task graph ---------> Step 7: physics scaling
        |
        v
Step 4: persistent GPU scene
        |
        v
Step 5: render graph/transfers
        |
        +-------------> Step 8: streaming/residency
        |
        v
Step 6: advanced image quality
        |
        v
Step 9: permanent tier gates
```

Step 9 instrumentation should be added incrementally throughout the work; the
final step formalizes it into release gates. Each earlier step remains useful
independently and must leave its compatible fallback intact until measured
coverage supports retiring it.

## Explicit non-goals until requirements exist

- Networking, rollback, and multiplayer prediction.
- A stable native binary-plugin ABI.
- Editor collaboration services.
- Unbounded physics feature expansion without representative game workloads.
- Renderer rewrites justified only by architecture preference rather than
  benchmark evidence.
