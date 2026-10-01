#include "engine/jobs/FrameTaskGraph.hpp"

#include <atomic>
#include <iostream>
#include <mutex>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    std::vector<int> values(1024);
    std::atomic_bool readBeforeWrite{false};
    const FrameResourceUse writeValues[]{FrameResourceUse{"scene.transforms", FrameResourceAccess::Write}};
    const FrameResourceUse readValues[]{FrameResourceUse{"scene.transforms", FrameResourceAccess::Read}};
    FrameTaskGraph graph(42, {.workerCount = 4, .minimumGrainSize = 64});
    graph.add(
        "update bounds", writeValues,
        [&](const FrameTaskContext& context) {
            for (size_t index = context.rangeBegin; index < context.rangeEnd; ++index) {
                values[index] = static_cast<int>(index + 1);
            }
        },
        values.size());
    graph.add(
        "visibility", readValues,
        [&](const FrameTaskContext& context) {
            for (size_t index = context.rangeBegin; index < context.rangeEnd; ++index) {
                if (values[index] == 0) {
                    readBeforeWrite.store(true, std::memory_order_relaxed);
                }
            }
        },
        values.size());
    const FrameTaskResult result = graph.execute();
    expect(result.success && !readBeforeWrite.load(), "resource hazards create deterministic dependencies");
    expect(result.stats.nodeCount == 2 && result.stats.jobCount == 32,
           "stable ranges use the configured minimum grain size");
    expect(result.stats.maximumQueueDepth > 0 && result.stats.workerUtilization >= 0.0,
           "task graph reports scheduling telemetry");

    FrameTaskExecutor persistentExecutor({.workerCount = 2, .minimumGrainSize = 8});
    std::atomic_uint32_t persistentRuns{0};
    for (uint64_t generation = 100; generation < 102; ++generation) {
        FrameTaskGraph persistentGraph(generation, {.minimumGrainSize = 8});
        persistentGraph.add(
            "persistent workers", {},
            [&](const FrameTaskContext& context) {
                expect(context.generation == generation, "persistent executor publishes the current generation");
                persistentRuns.fetch_add(1, std::memory_order_relaxed);
            },
            8);
        expect(persistentExecutor.execute(persistentGraph).success,
               "an engine-owned executor can run consecutive frame graphs");
    }
    expect(persistentRuns.load(std::memory_order_relaxed) == 2,
           "persistent execution runs each stable range exactly once");

    std::vector<int> order;
    FrameTaskGraph deterministic(7, {.workerCount = 8, .minimumGrainSize = 1, .deterministic = true});
    deterministic.add(
        "ordered", {}, [&](const FrameTaskContext& context) { order.push_back(static_cast<int>(context.rangeBegin)); },
        4);
    expect(deterministic.execute().success && order == std::vector<int>({0, 1, 2, 3}),
           "single-thread deterministic mode preserves stable range order");

    FrameTaskGraph cancelled(9);
    cancelled.add("stale frame", {}, [](const FrameTaskContext&) {});
    cancelled.cancel();
    const FrameTaskResult cancelledResult = cancelled.execute();
    expect(!cancelledResult && cancelledResult.cancelled, "cancelled frame generations cannot publish work");
    return failures == 0 ? 0 : 1;
}
