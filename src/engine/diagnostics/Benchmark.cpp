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
    sample("cpu.opaque", s.cpuOpaqueMilliseconds, "ms", budgets.opaque.cpuMilliseconds);
    sample("cpu.blended", s.cpuBlendedMilliseconds, "ms", budgets.blended.cpuMilliseconds);
    sample("cpu.bloom_downsample", s.cpuBloomMilliseconds, "ms", budgets.bloom.cpuMilliseconds);
    sample("cpu.post", s.cpuPostMilliseconds);
    sample("cpu.ui", s.cpuUiMilliseconds, "ms", budgets.ui.cpuMilliseconds);
    sample("cpu.upload", s.cpuUploadMilliseconds, "ms", budgets.upload.cpuMilliseconds);
    // Upload submissions complete synchronously today, so these belong to the
    // current CPU frame, unlike the delayed graphics-command-buffer samples.
    if (s.uploadTimingsValid) {
        sample("gpu.upload", s.gpuUploadMilliseconds, "ms", budgets.upload.gpuMilliseconds);
    }
    sample("cpu.frame_slot_wait", s.cpuFrameWaitMilliseconds);
    sample("cpu.acquire_wait", s.cpuAcquireMilliseconds);
    sample("cpu.present", s.cpuPresentMilliseconds);
    sample("draw_calls", s.drawCalls, "count");
    sample("triangles", static_cast<double>(s.triangleCount), "count");
    sample("visible", s.visibleCount, "count");
    sample("culled", s.culledCount, "count");
    sample("packet_rebuild", s.renderPacketsRebuilt ? 1.0 : 0.0, "count");
    sample("lights", s.localLightCount, "count");
    sample("discarded_light_assignments", s.discardedLightAssignments, "count");
    sample("upload_bytes", static_cast<double>(s.uploadBytes), "bytes");
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
