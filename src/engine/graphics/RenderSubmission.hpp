#pragma once

#include "Rendering.hpp"

#include <cstdint>
#include <span>
#include <vector>

enum class RenderSubmissionPath {
    CompatibleInstanced,
    IndirectPrototype
};

struct RenderSubmissionBenchmark {
    double compatibleMilliseconds{0.0};
    double indirectMilliseconds{0.0};
};

struct RenderBatch {
    uint32_t indexCount{0};
    uint32_t instanceCount{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
    uint32_t firstInstance{0};
};

struct IndirectDrawCommand {
    uint32_t indexCount{0};
    uint32_t instanceCount{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
    uint32_t firstInstance{0};
};

RenderSubmissionPath chooseRenderSubmissionPath(const RenderCapabilities& capabilities,
                                                const RenderSubmissionBenchmark& benchmark,
                                                double requiredImprovement = 0.10);
std::vector<IndirectDrawCommand> buildIndirectPrototype(std::span<const RenderBatch> batches,
                                                        const RenderCapabilities& capabilities);
