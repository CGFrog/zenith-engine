#include "engine/graphics/graph/RenderGraph.hpp"

#include <iostream>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

RenderGraphImageDescription image(uint32_t format, uint64_t usage, uint64_t bytes) {
    return {.format = format,
            .width = 1280,
            .height = 720,
            .usage = usage,
            .requestedBytes = bytes,
            .alignment = 256,
            .memoryTypeBits = 3};
}
} // namespace

int main() {
    RenderGraph graph;
    const auto swapchain = graph.importImage("swapchain", image(1, 3, 0), 0);
    const auto depth = graph.importImage("depth", image(2, 5, 0), 0);
    const auto hdr = graph.createTransientImage("hdr", image(3, 7, 8 * 1024 * 1024));
    const auto bloom = graph.createTransientImage("bloom", image(3, 7, 2 * 1024 * 1024));
    const auto scratch = graph.createTransientImage("scratch", image(3, 7, 4 * 1024 * 1024));

    const RenderGraphAccess opaqueAccesses[]{{hdr, RenderGraphAccessMode::Write, 1, 1, 1},
                                             {depth, RenderGraphAccessMode::Write, 1, 2, 2}};
    const auto opaque = graph.addPass("opaque", RenderGraphQueue::Graphics, opaqueAccesses);
    const RenderGraphAccess bloomAccesses[]{{hdr, RenderGraphAccessMode::Read, 2, 4, 3},
                                            {bloom, RenderGraphAccessMode::Write, 2, 8, 4}};
    graph.addPass("bloom", RenderGraphQueue::Compute, bloomAccesses);
    const RenderGraphAccess postAccesses[]{{bloom, RenderGraphAccessMode::Read, 4, 4, 3},
                                           {swapchain, RenderGraphAccessMode::Write, 4, 1, 1}};
    graph.addPass("post", RenderGraphQueue::Graphics, postAccesses);
    const RenderGraphAccess lateAccesses[]{{scratch, RenderGraphAccessMode::Write, 2, 8, 4}};
    const RenderGraphPassHandle lateDependencies[]{opaque};
    graph.addPass("late scratch", RenderGraphQueue::Compute, lateAccesses, lateDependencies);

    const RenderGraphCompileResult compiled = graph.compile();
    expect(compiled.success, "a valid render graph compiles");
    expect(compiled.graph.executionOrder.size() == 4, "all passes appear in the execution plan");
    expect(compiled.graph.statistics.queueTransferCount >= 2,
           "cross-queue producer/consumer hazards create ownership transfers");
    expect(compiled.graph.statistics.requestedTransientBytes == 14 * 1024 * 1024,
           "requested transient bytes include every graph-owned resource");
    expect(compiled.graph.statistics.committedTransientBytes == 10 * 1024 * 1024,
           "compatible non-overlapping resources alias one physical slot");
    expect(compiled.graph.statistics.aliasedTransientBytes == 4 * 1024 * 1024, "alias savings are reported explicitly");

    const RenderGraphCompileResult conservative = graph.compile(true);
    expect(conservative.graph.statistics.barrierCount >= compiled.graph.statistics.barrierCount,
           "conservative validation never emits fewer barriers than hazard synchronization");

    RenderGraph incompatible;
    const auto first = incompatible.createTransientImage("first", image(1, 1, 4096));
    const auto second = incompatible.createTransientImage("second", image(2, 1, 4096));
    const RenderGraphAccess firstAccess[]{first, RenderGraphAccessMode::Write, 1, 1, 1};
    const RenderGraphAccess secondAccess[]{second, RenderGraphAccessMode::Write, 1, 1, 1};
    incompatible.addPass("first", RenderGraphQueue::Graphics, firstAccess);
    incompatible.addPass("second", RenderGraphQueue::Graphics, secondAccess);
    const auto incompatibleResult = incompatible.compile();
    expect(incompatibleResult.graph.statistics.committedTransientBytes == 8192,
           "different image formats cannot alias even when lifetimes do not overlap");

    RenderGraph cycle;
    const RenderGraphPassHandle future{1};
    const auto cycleFirst = cycle.addPass("first", RenderGraphQueue::Graphics, {}, std::span{&future, 1});
    cycle.addPass("second", RenderGraphQueue::Graphics, {}, std::span{&cycleFirst, 1});
    expect(!cycle.compile(), "explicit dependency cycles are rejected");

    expect(!shouldEnableAsyncCompute({.separateComputeQueue = false,
                                      .serialGpuMilliseconds = 4.0,
                                      .asynchronousGpuMilliseconds = 3.5,
                                      .measuredOverlapMilliseconds = 0.5}),
           "shared queue topologies keep compute work on graphics");
    expect(!shouldEnableAsyncCompute({.separateComputeQueue = true,
                                      .serialGpuMilliseconds = 4.0,
                                      .asynchronousGpuMilliseconds = 3.95,
                                      .measuredOverlapMilliseconds = 0.4}),
           "noise-sized async compute gains do not enable the path");
    expect(shouldEnableAsyncCompute({.separateComputeQueue = true,
                                     .serialGpuMilliseconds = 4.0,
                                     .asynchronousGpuMilliseconds = 3.7,
                                     .measuredOverlapMilliseconds = 0.4}),
           "a separate queue with measured overlap and material gain enables async compute");
    return failures == 0 ? 0 : 1;
}
