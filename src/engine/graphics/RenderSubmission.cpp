#include "RenderSubmission.hpp"

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
