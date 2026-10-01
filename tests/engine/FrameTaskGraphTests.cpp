#include "engine/jobs/FrameTaskGraph.hpp"

#include <algorithm>
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

    for (size_t workerCount : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{0}}) {
        FrameTaskExecutor executor({.workerCount = workerCount, .minimumGrainSize = 17});
        std::vector<uint32_t> visits(997, 0);
        FrameTaskGraph workerGraph(200 + workerCount, {.minimumGrainSize = 17});
        workerGraph.add(
            "worker-count matrix", {},
            [&](const FrameTaskContext& context) {
                for (size_t index = context.rangeBegin; index < context.rangeEnd; ++index) {
                    ++visits[index];
                }
            },
            visits.size());
        const FrameTaskResult workerResult = executor.execute(workerGraph);
        expect(workerResult.success && std::ranges::all_of(visits, [](uint32_t visitCount) { return visitCount == 1; }),
               "1, 2, 4, 8, and automatic workers cover every stable range exactly once");
    }

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

    FrameTaskExecutor transitionExecutor({.workerCount = 4, .minimumGrainSize = 4});
    for (uint64_t generation = 0; generation < 128; ++generation) {
        FrameTaskGraph transition(generation, {.minimumGrainSize = 4});
        std::atomic_uint32_t published{0};
        transition.add(
            "transition stress", {},
            [&](const FrameTaskContext& context) {
                if (!context.cancellationRequested()) {
                    published.fetch_add(1, std::memory_order_relaxed);
                }
            },
            64);
        if ((generation % 3) == 0) {
            transition.cancel();
        }
        const FrameTaskResult transitionResult = transitionExecutor.execute(transition);
        expect((generation % 3) == 0 ? transitionResult.cancelled : transitionResult.success,
               "transition stress preserves cancellation and generation state");
        expect((generation % 3) == 0 ? published.load() == 0 : published.load() == 16,
               "cancelled transitions cannot publish stale range outputs");
    }
    return failures == 0 ? 0 : 1;
}
