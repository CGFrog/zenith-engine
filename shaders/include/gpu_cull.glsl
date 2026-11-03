// One GPU-driven draw batch, read by the visibility and compaction passes.
// Mirrors GpuDrivenBatch in engine/graphics/passes/GpuDrivenPass.hpp (std430).
#ifndef ZENITH_GPU_CULL_GLSL
#define ZENITH_GPU_CULL_GLSL

struct BatchRecord {
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
    uint group;
    uint groupFirstBatch;
    float lodGeometricError;
    uint nextLodBatch;
};

#endif
