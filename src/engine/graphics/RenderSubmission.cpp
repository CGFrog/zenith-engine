#include "RenderSubmission.hpp"

#include <algorithm>
#include <numeric>

RenderSubmissionPath chooseRenderSubmissionPath(const RenderCapabilities& capabilities,
                                                const RenderSubmissionBenchmark& benchmark,
                                                double requiredImprovement) {
    const bool supported =
        capabilities.descriptorIndexing && capabilities.multiDrawIndirect && capabilities.indirectDrawCount;
    if (!supported || benchmark.compatibleMilliseconds <= 0.0 || benchmark.indirectMilliseconds <= 0.0) {
        return RenderSubmissionPath::CompatibleInstanced;
    }
    const double improvement =
        (benchmark.compatibleMilliseconds - benchmark.indirectMilliseconds) / benchmark.compatibleMilliseconds;
    return improvement >= requiredImprovement ? RenderSubmissionPath::IndirectPrototype
                                              : RenderSubmissionPath::CompatibleInstanced;
}

std::vector<IndirectDrawCommand> buildIndirectPrototype(std::span<const RenderBatch> batches,
                                                        const RenderCapabilities& capabilities) {
    if (!capabilities.descriptorIndexing || !capabilities.multiDrawIndirect || !capabilities.indirectDrawCount) {
        return {};
    }
    std::vector<IndirectDrawCommand> commands;
    commands.reserve(batches.size());
    for (const RenderBatch& batch : batches) {
        commands.push_back(
            {batch.indexCount, batch.instanceCount, batch.firstIndex, batch.vertexOffset, batch.firstInstance});
    }
    return commands;
}

std::vector<size_t> stableRenderOrder(std::span<const RenderOrderKey> keys) {
    std::vector<size_t> order(keys.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [keys](size_t first, size_t second) {
        const bool firstBlended = keys[first].surface == RenderSurfaceClass::Blended;
        const bool secondBlended = keys[second].surface == RenderSurfaceClass::Blended;
        if (firstBlended != secondBlended) {
            return !firstBlended;
        }
        return firstBlended && keys[first].cameraDistanceSquared > keys[second].cameraDistanceSquared;
    });
    return order;
}
