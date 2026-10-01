# Streaming and hitch validation

Step 8 has two reproducible validation layers.

The CPU-only residency replay is deterministic and runs on every machine:

```powershell
ctest --test-dir build -C Debug --output-on-failure -R zenith_asset_streaming
```

It follows a fixed six-zone camera path twice. The cold traversal records p99
and maximum modeled frame cost, frames over budget, decoded and uploaded bytes,
backlog, residency misses, and CPU/GPU residency peaks. The second traversal
uses the populated cache and must perform no decode or upload work. The same
test verifies combined request priority, per-frame byte/job limits, independent
texture-mip and mesh-chunk residency, ordered memory-pressure recovery, serial
retirement, hard-budget failure, and rejection of stale cancelled completions.
`zenith_production_milestone_tests` separately exercises deterministic additive
scene priority, desired-set unload, repeated load/unload, and cancellation.

For GPU and driver behavior, build Release and capture two identical bounded
runs. Remove the platform `pipeline-cache.bin` before the first run only to
force the cold case; its location is the SDL preference directory returned for
organization and application `Zenith`.

```powershell
cmake --build build --config Release --parallel 4
$env:ZENITH_BENCHMARK_FRAMES='500'
$env:ZENITH_BENCHMARK_WARMUP_FRAMES='120'
$env:ZENITH_BENCHMARK_SAMPLES='1'
$env:ZENITH_BENCHMARK_FRAME_BUDGET_MS='8.333'
build\Release\zenith.exe > cold-streaming.json
build\Release\zenith.exe > warm-streaming.json
```

Compare only reports from the same build, driver, GPU, power state, resolution,
and presentation mode. In addition to whole-frame p99, maximum, and
`over_budget`, inspect `streaming.*`, `upload.backpressure_events`, and
`pipeline.*`. A warm run should report `pipeline.cache_warm = 1`; creation and
first-use timing remain visible even when cache-backed. Backpressure is an
accounted deferral and should not coincide with a new frame-slot or queue-idle
wait. A passing traversal has no hard-budget failures, a drained backlog after
the path settles, bounded residency peaks, and no content resurrection after
scene cancellation.
