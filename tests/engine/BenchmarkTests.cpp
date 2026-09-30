#include "engine/diagnostics/Benchmark.hpp"
#include "engine/diagnostics/Timestamp.hpp"
#include "engine/graphics/Rendering.hpp"

#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main() {
    try {
        const std::vector<double> samples{9.0,  1.0,  5.0,  3.0,  7.0,  11.0, 13.0, 15.0, 17.0, 19.0,
                                          21.0, 23.0, 25.0, 27.0, 29.0, 31.0, 33.0, 35.0, 37.0, 39.0};
        const SampleSummary summary = summarizeSamples(samples, 30.0);
        require(summary.count == 20, "sample count was not preserved");
        require(summary.median == 20.0, "even-sized median is incorrect");
        require(summary.p95 == 37.0, "nearest-rank p95 is incorrect");
        require(summary.p99 == 39.0, "nearest-rank p99 is incorrect");
        require(summary.maximum == 39.0, "maximum is incorrect");
        require(summary.overBudget == 5, "budget miss count is incorrect");

        require(std::abs(timestampMilliseconds(250, 5, 8, 1'000.0) - 0.011) < 1e-9, "timestamp wrap was not handled");
        require(timestampMilliseconds(10, 20, 0, 1.0) == 0.0, "unsupported timestamps should report zero");

        BenchmarkReport report;
        RenderPassBudgets budgets;
        budgets.shadow.gpuMilliseconds = 0.5;
        RenderStats stats;
        stats.gpuTimingsValid = true;
        stats.gpuSampleFrame = 42;
        stats.gpuFrameMilliseconds = 1.25;
        stats.gpuShadowMilliseconds = 0.75;
        report.renderGpu(stats, budgets, 40, 50);
        report.renderGpu(stats, budgets, 40, 50);
        const nlohmann::json output = nlohmann::json::parse(report.json(true));
        require(output["schema_version"] == 2, "benchmark schema version changed unexpectedly");
        require(output["metrics"]["gpu.frame"]["count"] == 1, "retired GPU frame was sampled twice");
        require(output["metrics"]["gpu.shadow"]["over_budget"] == 1, "GPU budget miss was not recorded");
        require(output["gpu_sample_frames"].size() == 1, "GPU sample identity was not retained");
    } catch (const std::exception& error) {
        std::cerr << "Benchmark test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
