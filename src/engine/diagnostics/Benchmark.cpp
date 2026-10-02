#include "Benchmark.hpp"

#include "engine/graphics/Rendering.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <numeric>
#include <stdexcept>
#include <thread>

SampleSummary summarizeSamples(std::span<const double> samples, double budget) {
    if (!std::isfinite(budget) || budget < 0.0) {
        throw std::invalid_argument("Invalid benchmark budget");
    }
    std::vector<double> sorted(samples.begin(), samples.end());
    for (double value : sorted) {
        if (!std::isfinite(value) || value < 0.0) {
            throw std::invalid_argument("Benchmark samples must be finite and nonnegative");
        }
    }
    if (sorted.empty()) {
        return {};
    }
    std::ranges::sort(sorted);
    const auto percentile = [&](double fraction) {
        return sorted[static_cast<size_t>(std::ceil(fraction * sorted.size())) - 1];
    };
    const size_t middle = sorted.size() / 2;
    return {.count = sorted.size(),
            .mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size(),
            .median = sorted.size() % 2 == 0 ? std::midpoint(sorted[middle - 1], sorted[middle]) : sorted[middle],
            .p95 = percentile(0.95),
            .p99 = percentile(0.99),
            .maximum = sorted.back(),
            .overBudget =
                budget > 0.0
                    ? static_cast<uint64_t>(std::ranges::count_if(sorted, [=](double value) { return value > budget; }))
                    : 0};
}

void BenchmarkReport::metadata(std::string key, std::string value) {
    m_metadata.insert_or_assign(std::move(key), std::move(value));
}

void BenchmarkReport::sample(const std::string& name, double value, std::string unit, double budget) {
    if (!std::isfinite(value) || value < 0.0 || !std::isfinite(budget) || budget < 0.0) {
        throw std::invalid_argument("Invalid benchmark sample or budget: " + name);
    }
    auto [entry, inserted] = m_metrics.try_emplace(name, Metric{unit, budget, {}});
    if (!inserted && (entry->second.unit != unit || entry->second.budget != budget)) {
        throw std::invalid_argument("Benchmark metric changed units or budget: " + name);
    }
    entry->second.values.push_back(value);
}

void BenchmarkReport::renderCpu(const RenderStats& s, const RenderPassBudgets& budgets) {
    sample("cpu.render_packet", s.cpuRenderPacketMilliseconds);
    sample("cpu.shadow", s.cpuShadowMilliseconds, "ms", budgets.shadow.cpuMilliseconds);
    sample("cpu.local_shadows", s.cpuLocalShadowMilliseconds, "ms", budgets.localShadows.cpuMilliseconds);
    sample("cpu.opaque", s.cpuOpaqueMilliseconds, "ms", budgets.opaque.cpuMilliseconds);
    sample("cpu.blended", s.cpuBlendedMilliseconds, "ms", budgets.blended.cpuMilliseconds);
    sample("cpu.bloom_downsample", s.cpuBloomMilliseconds, "ms", budgets.bloom.cpuMilliseconds);
    sample("cpu.post", s.cpuPostMilliseconds);
    sample("cpu.ui", s.cpuUiMilliseconds, "ms", budgets.ui.cpuMilliseconds);
    sample("cpu.upload", s.cpuUploadMilliseconds, "ms", budgets.upload.cpuMilliseconds);
    sample("cpu.temporal_resolve", s.cpuPostMilliseconds, "ms", budgets.temporalResolve.cpuMilliseconds);
    // Upload timings are retired from the bounded staging ring, unlike the
    // delayed graphics-command-buffer samples.
    if (s.uploadTimingsValid) {
        sample("gpu.upload", s.gpuUploadMilliseconds, "ms", budgets.upload.gpuMilliseconds);
    }
    sample("cpu.frame_slot_wait", s.cpuFrameWaitMilliseconds);
    sample("cpu.acquire_wait", s.cpuAcquireMilliseconds);
    sample("cpu.present", s.cpuPresentMilliseconds);
    sample("draw_calls", s.drawCalls, "count");
    sample("triangles", static_cast<double>(s.triangleCount), "count");
    sample("shadow.cascades_rendered", s.shadowCascadesRendered, "count");
    sample("shadow.cascades_cached", s.shadowCascadesCached, "count");
    sample("visible", s.visibleCount, "count");
    sample("culled", s.culledCount, "count");
    sample("packet_rebuild", s.renderPacketsRebuilt ? 1.0 : 0.0, "count");
    sample("renderables_scanned", s.scannedRenderables, "count");
    sample("object_records_updated", s.updatedObjectRecords, "count");
    sample("main_thread_allocations", s.mainThreadAllocations, "count");
    sample("indirect_commands", s.indirectCommandCount, "count");
    sample("gpu_visibility_mismatches", s.gpuVisibilityMismatches, "count");
    sample("gpu.culling", s.gpuCullingMilliseconds);
    sample("tasks.queue_depth", s.taskQueueDepth, "count");
    sample("tasks.stolen_jobs", s.taskStolenJobs, "count");
    sample("tasks.worker_utilization", s.taskWorkerUtilization, "ratio");
    sample("tasks.main_thread_assist", s.taskMainThreadAssistMilliseconds);
    sample("tasks.graph_latency", s.taskGraphLatencyMilliseconds);
    sample("lights", s.localLightCount, "count");
    sample("reflection_probes", s.reflectionProbeCount, "count");
    sample("discarded_light_assignments", s.discardedLightAssignments, "count");
    sample("upload_bytes", static_cast<double>(s.uploadBytes), "bytes");
    sample("upload.backpressure_events", s.uploadBackpressureEvents, "count");
    sample("render_graph.barriers", s.renderGraphBarrierCount, "count");
    sample("render_graph.queue_transfers", s.renderGraphQueueTransfers, "count");
    sample("render_graph.transient_requested", static_cast<double>(s.transientRequestedBytes), "bytes");
    sample("render_graph.transient_committed", static_cast<double>(s.transientCommittedBytes), "bytes");
    sample("render_graph.transient_aliased", static_cast<double>(s.transientAliasedBytes), "bytes");
    sample("render_graph.transient_peak", static_cast<double>(s.transientPeakBytes), "bytes");
    sample("render.internal_width", s.internalRenderWidth, "pixels");
    sample("render.internal_height", s.internalRenderHeight, "pixels");
    sample("render.internal_scale", s.internalResolutionScale, "ratio");
    sample("render.temporal_history_bytes", static_cast<double>(s.temporalHistoryBytes), "bytes");
    sample("render.motion_vector_bytes", static_cast<double>(s.motionVectorBytes), "bytes");
    sample("render.ambient_occlusion_bytes", static_cast<double>(s.ambientOcclusionBytes), "bytes");
    sample("render.reflection_bytes", static_cast<double>(s.reflectionBytes), "bytes");
    sample("render.shadow_cache_bytes", static_cast<double>(s.shadowCacheBytes), "bytes");
    sample("render.local_shadow_atlas_bytes", static_cast<double>(s.localShadowAtlasBytes), "bytes");
    sample("local_shadows.tiles_rendered", s.localShadowTilesRendered, "count");
    sample("local_shadows.tiles_cached", s.localShadowTilesCached, "count");
    sample("local_shadows.tiles_pending", s.localShadowTilesPending, "count");
    sample("local_shadows.lights_rejected", s.localShadowLightsRejected, "count");
    sample("render.volumetric_fog_bytes", static_cast<double>(s.volumetricFogBytes), "bytes");
    sample("render.irradiance_volume_probes", s.irradianceVolumeProbes, "count");
    sample("render.irradiance_volume_bytes", static_cast<double>(s.irradianceVolumeBytes), "bytes");
    sample("render.motion_blur_bytes", static_cast<double>(s.motionBlurBytes), "bytes");
    sample("streaming.decoded_bytes", static_cast<double>(s.streamingDecodedBytes), "bytes");
    sample("streaming.uploaded_bytes", static_cast<double>(s.streamingUploadedBytes), "bytes");
    sample("streaming.decode_backlog_bytes", static_cast<double>(s.streamingDecodeBacklogBytes), "bytes");
    sample("streaming.upload_backlog_bytes", static_cast<double>(s.streamingUploadBacklogBytes), "bytes");
    sample("streaming.cpu_peak_bytes", static_cast<double>(s.streamingCpuPeakBytes), "bytes");
    sample("streaming.gpu_peak_bytes", static_cast<double>(s.streamingGpuPeakBytes), "bytes");
    sample("streaming.residency_misses", s.streamingResidencyMisses, "count");
    sample("streaming.hard_budget_failures", s.streamingHardBudgetFailures, "count");
    sample("pipeline.cache_warm", s.pipelineCacheWarm ? 1.0 : 0.0, "count");
    sample("pipeline.creation_events", s.pipelineCreationEvents, "count");
    sample("pipeline.creation_time", s.pipelineCreationMilliseconds);
    sample("pipeline.first_use_events", s.pipelineFirstUseEvents, "count");
    sample("pipeline.first_use_time", s.pipelineFirstUseMilliseconds);
    // Silent-fallback audit: nonzero at steady state means content is missing.
    sample("upload.deferred_meshes", s.deferredMeshUploads, "count");
    sample("upload.deferred_textures", s.deferredTextureUploads, "count");
    sample("lights.dropped", s.droppedLocalLights, "count");
}

void BenchmarkReport::renderGpu(const RenderStats& s, const RenderPassBudgets& budgets, uint64_t firstFrame,
                                uint64_t lastFrame) {
    if (!s.gpuTimingsValid || s.gpuSampleFrame < firstFrame || s.gpuSampleFrame > lastFrame ||
        std::ranges::find(m_gpuFrames, s.gpuSampleFrame) != m_gpuFrames.end()) {
        return;
    }
    m_gpuFrames.push_back(s.gpuSampleFrame);
    sample("gpu.frame", s.gpuFrameMilliseconds);
    sample("gpu.shadow", s.gpuShadowMilliseconds, "ms", budgets.shadow.gpuMilliseconds);
    sample("gpu.opaque", s.gpuOpaqueMilliseconds, "ms", budgets.opaque.gpuMilliseconds);
    sample("gpu.blended", s.gpuBlendedMilliseconds, "ms", budgets.blended.gpuMilliseconds);
    sample("gpu.bloom_downsample", s.gpuBloomMilliseconds, "ms", budgets.bloom.gpuMilliseconds);
    sample("gpu.temporal_resolve", s.gpuTemporalMilliseconds, "ms", budgets.temporalResolve.gpuMilliseconds);
    sample("gpu.ambient_occlusion", s.gpuAmbientOcclusionMilliseconds, "ms", budgets.ambientOcclusion.gpuMilliseconds);
    sample("gpu.reflections", s.gpuReflectionsMilliseconds, "ms", budgets.reflections.gpuMilliseconds);
    sample("gpu.exposure", s.gpuExposureMilliseconds, "ms", budgets.exposure.gpuMilliseconds);
    sample("gpu.depth_prepass", s.gpuDepthPrepassMilliseconds, "ms", budgets.depthPrepass.gpuMilliseconds);
    sample("gpu.depth_pyramid", s.gpuDepthPyramidMilliseconds, "ms", budgets.depthPyramid.gpuMilliseconds);
    sample("gpu.light_clusters", s.gpuLightClusterMilliseconds, "ms", budgets.lightClusters.gpuMilliseconds);
    sample("gpu.environment", s.gpuEnvironmentPrecomputeMilliseconds + s.gpuSkyboxMilliseconds, "ms",
           budgets.environment.gpuMilliseconds);
    sample("gpu.environment_precompute", s.gpuEnvironmentPrecomputeMilliseconds);
    sample("gpu.skybox", s.gpuSkyboxMilliseconds);
    sample("gpu.shadow_culling", s.gpuShadowCullingMilliseconds, "ms", budgets.shadowCulling.gpuMilliseconds);
    sample("gpu.local_shadows", s.gpuLocalShadowMilliseconds, "ms", budgets.localShadows.gpuMilliseconds);
    sample("gpu.reflection_probe_clusters", s.gpuReflectionProbeClusterMilliseconds, "ms",
           budgets.reflectionProbeClusters.gpuMilliseconds);
    sample("gpu.volumetric_fog", s.gpuVolumetricFogMilliseconds, "ms", budgets.volumetricFog.gpuMilliseconds);
    sample("gpu.irradiance_volume", s.gpuIrradianceVolumeMilliseconds, "ms", budgets.irradianceVolume.gpuMilliseconds);
    sample("gpu.motion_blur", s.gpuMotionBlurMilliseconds, "ms", budgets.motionBlur.gpuMilliseconds);
    sample("gpu.upload_wait", s.gpuUploadWaitMilliseconds);
    sample("gpu.post", s.gpuPostMilliseconds);
    sample("gpu.ui", s.gpuUiMilliseconds, "ms", budgets.ui.gpuMilliseconds);
}

void BenchmarkReport::physics(const PhysicsStats& s) {
    sample("cpu.physics_transforms", s.transformMilliseconds);
    sample("cpu.physics_sync_in", s.synchronizeInMilliseconds);
    sample("cpu.physics_jolt", s.simulationMilliseconds);
    sample("cpu.physics_sync_out", s.synchronizeOutMilliseconds);
    sample("physics.bodies", s.bodies, "count");
    sample("physics.active", s.activeBodies, "count");
    sample("physics.sleeping", s.sleepingBodies, "count");
    sample("physics.contact_events", s.contactEvents, "count");
    sample("physics.lifecycle_operations", s.lifecycleOperations, "count");
    sample("physics.audited_entities", s.auditedEntities, "count");
    sample("physics.transforms_read_back", s.transformsReadBack, "count");
}

std::string BenchmarkReport::json(bool includeSamples) const {
    nlohmann::json result{{"schema_version", 2},
                          {"metadata", m_metadata},
                          {"metrics", nlohmann::json::object()},
                          {"gpu_sample_frames", m_gpuFrames}};
    for (const auto& [name, metric] : m_metrics) {
        const auto s = summarizeSamples(metric.values, metric.budget);
        auto& output = result["metrics"][name];
        output = {{"unit", metric.unit},
                  {"count", s.count},
                  {"mean", s.mean},
                  {"median", s.median},
                  {"p95", s.p95},
                  {"p99", s.p99},
                  {"max", s.maximum},
                  {"budget", metric.budget},
                  {"over_budget", s.overBudget}};
        if (includeSamples) {
            output["samples"] = metric.values;
        }
    }
    return result.dump();
}

void addBuildMetadata(BenchmarkReport& report) {
    report.metadata("source_revision", ZENITH_BENCHMARK_REVISION);
    report.metadata("source_dirty_at_configure", ZENITH_BENCHMARK_DIRTY);
    report.metadata("build", ZENITH_BENCHMARK_BUILD);
    report.metadata("compiler", ZENITH_BENCHMARK_COMPILER);
    report.metadata("platform", ZENITH_BENCHMARK_PLATFORM);
    report.metadata("hardware_threads", std::to_string(std::thread::hardware_concurrency()));
}
