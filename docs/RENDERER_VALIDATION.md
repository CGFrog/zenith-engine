# Renderer validation

Milestone 8 keeps renderer changes measurable and capability-gated.

## Performance

`zenith_render_packet_benchmark` constructs a deterministic 2,000-object scene
and reports JSON timings for the previous rebuild-and-sort work versus cached
packet revision checks. `RenderStats::cpuRenderPacketMilliseconds` also exposes
the live rebuild cost and whether packets were rebuilt. For whole-frame checks,
run the same executable, machine, driver, present mode, and scene twice:

```powershell
$env:ZENITH_BENCHMARK_OBJECTS='2000'
$env:ZENITH_BENCHMARK_FRAMES='500'
$env:ZENITH_DISABLE_RENDER_PACKET_CACHE='1'; build\Release\zenith.exe # baseline
Remove-Item Env:ZENITH_DISABLE_RENDER_PACKET_CACHE
build\Release\zenith.exe # candidate
```

Record median and p95 CPU/GPU values from the JSON output. A candidate fails
the milestone gate if default-quality median or p95 regresses by more than 5%
outside run-to-run noise.

The 2026-09-27 Debug validation on the same machine and driver used 200 warm
measurements after the built-in 1,000-frame warmup:

| 2,000 objects | CPU median | CPU p95 | GPU median | GPU p95 | packet build |
| --- | ---: | ---: | ---: | ---: | ---: |
| cache disabled | 16.163 ms | 16.688 ms | 0.508 ms | 0.512 ms | 3.371 ms |
| cache enabled | 12.843 ms | 13.355 ms | 0.508 ms | 0.511 ms | 0.0002 ms |

The candidate improved CPU median by 20.5% and p95 by 20.0%; GPU timing stayed
within 0.3%. The default-quality regression gate therefore passed.

The indirect command prototype encodes the 2,000-object workload only when
descriptor indexing, multi-draw indirect, and indirect draw count are all
available. The compatible instanced path remains selected unless a same-device
benchmark improves by at least 10%. The prototype is not selected by default.

## Image stability

`zenith_render_regression` compares deterministic color-space, cascade-shadow,
edge-antialiasing, and final game-UI composition captures with sealed hashes.
The UI capture covers the post-tone-map ordering and expected output color
space. On failure it writes the candidate PPM images to the system temporary
directory for inspection.

The public anti-aliasing choices remain **Off** and **FXAA**. Temporal AA is
deliberately not exposed: adding it requires motion vectors plus automated
disocclusion, static-camera stability, and moving-camera ghosting captures.
This gate prevents an FXAA implementation from being mislabeled as TAA.

## Vulkan smoke

The `zenith_editor_vulkan_smoke` CTest runs a bounded editor scenario that
resizes the offscreen viewport twice and exercises Play, Pause, Single Step,
and Stop. It is labeled `vulkan;smoke` so headless CI can exclude it while a
Vulkan-capable workstation runs it explicitly.
