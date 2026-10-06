#pragma once

// Benchmark sampling and the JSON report (ZENITH_BENCHMARK_*).

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

struct RenderStats;
struct PhysicsStats;
struct RenderPassBudgets;

struct SampleSummary {
    uint64_t count{0};
    double mean{0.0};
    double median{0.0};
    double p95{0.0};
    double p99{0.0};
    double maximum{0.0};
    uint64_t overBudget{0};
};

// Median averages the two middle samples; tail percentiles use nearest rank.
SampleSummary summarizeSamples(std::span<const double> samples, double budget = 0.0);

// Bounded by the caller's measured frame count. Raw samples preserve spikes and
// correlation; aggregation happens after measurement. Missing metrics are absent.
class BenchmarkReport {
  public:
    void metadata(std::string key, std::string value);
    void sample(const std::string& name, double value, std::string unit = "ms", double budget = 0.0);
    void renderCpu(const RenderStats& stats, const RenderPassBudgets& budgets);
    void renderGpu(const RenderStats& stats, const RenderPassBudgets& budgets, uint64_t firstFrame, uint64_t lastFrame);
    void physics(const PhysicsStats& stats);
    std::string json(bool includeSamples = false) const;

  private:
    struct Metric {
        std::string unit;
        double budget{0.0};
        std::vector<double> values;
    };

    std::map<std::string, std::string> m_metadata;
    std::map<std::string, Metric> m_metrics;
    std::vector<uint64_t> m_gpuFrames;
};

void addBuildMetadata(BenchmarkReport& report);
