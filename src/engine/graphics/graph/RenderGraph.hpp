#pragma once

// Declarative render-graph model: passes, resources, hazards, barrier statistics.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class RenderGraphQueue : uint8_t {
    Graphics,
    Compute,
    Transfer,
};

enum class RenderGraphResourceKind : uint8_t {
    Image,
    Buffer,
};

enum class RenderGraphAccessMode : uint8_t {
    Read,
    Write,
    ReadWrite,
};

struct RenderGraphResourceHandle {
    uint32_t index{UINT32_MAX};

    explicit operator bool() const {
        return index != UINT32_MAX;
    }

    auto operator<=>(const RenderGraphResourceHandle&) const = default;
};

struct RenderGraphPassHandle {
    uint32_t index{UINT32_MAX};

    explicit operator bool() const {
        return index != UINT32_MAX;
    }

    auto operator<=>(const RenderGraphPassHandle&) const = default;
};

struct RenderGraphImageDescription {
    uint32_t format{0};
    uint32_t width{1};
    uint32_t height{1};
    uint32_t depth{1};
    uint32_t mipLevels{1};
    uint32_t arrayLayers{1};
    uint32_t samples{1};
    uint64_t usage{0};
    uint64_t requestedBytes{0};
    uint64_t alignment{1};
    uint32_t memoryTypeBits{UINT32_MAX};
};

struct RenderGraphBufferDescription {
    uint64_t size{0};
    uint64_t usage{0};
    uint64_t alignment{1};
    uint32_t memoryTypeBits{UINT32_MAX};
};

struct RenderGraphAccess {
    RenderGraphResourceHandle resource;
    RenderGraphAccessMode mode{RenderGraphAccessMode::Read};
    uint64_t stages{0};
    uint64_t access{0};
    uint32_t layout{0};
};

struct RenderGraphBarrier {
    RenderGraphResourceHandle resource;
    RenderGraphPassHandle sourcePass;
    RenderGraphPassHandle destinationPass;
    RenderGraphQueue sourceQueue{RenderGraphQueue::Graphics};
    RenderGraphQueue destinationQueue{RenderGraphQueue::Graphics};
    uint64_t sourceStages{0};
    uint64_t destinationStages{0};
    uint64_t sourceAccess{0};
    uint64_t destinationAccess{0};
    uint32_t oldLayout{0};
    uint32_t newLayout{0};
    bool queueOwnershipTransfer{false};
};

struct RenderGraphAllocation {
    RenderGraphResourceHandle resource;
    uint32_t slot{UINT32_MAX};
    uint64_t offset{0};
    uint64_t size{0};
    uint32_t firstPass{0};
    uint32_t lastPass{0};
    bool aliased{false};
};

struct RenderGraphStatistics {
    uint32_t passCount{0};
    uint32_t barrierCount{0};
    uint32_t queueTransferCount{0};
    uint64_t requestedTransientBytes{0};
    uint64_t committedTransientBytes{0};
    uint64_t aliasedTransientBytes{0};
    uint64_t peakTransientBytes{0};
};

struct CompiledRenderGraph {
    std::vector<RenderGraphPassHandle> executionOrder;
    std::vector<RenderGraphBarrier> barriers;
    std::vector<RenderGraphAllocation> allocations;
    RenderGraphStatistics statistics;
};

struct RenderGraphCompileResult {
    bool success{false};
    std::string error;
    CompiledRenderGraph graph;

    explicit operator bool() const {
        return success;
    }
};

struct AsyncComputeMeasurements {
    bool separateComputeQueue{false};
    double serialGpuMilliseconds{0.0};
    double asynchronousGpuMilliseconds{0.0};
    double measuredOverlapMilliseconds{0.0};
    double minimumRequiredGainMilliseconds{0.10};
};

// Async compute is a measured optimization, never a capability-only choice.
bool shouldEnableAsyncCompute(const AsyncComputeMeasurements& measurements);

class RenderGraph {
  public:
    RenderGraphResourceHandle importImage(std::string name, const RenderGraphImageDescription& description,
                                          uint32_t initialLayout, RenderGraphQueue owner = RenderGraphQueue::Graphics);
    RenderGraphResourceHandle importBuffer(std::string name, const RenderGraphBufferDescription& description,
                                           RenderGraphQueue owner = RenderGraphQueue::Graphics);
    RenderGraphResourceHandle createTransientImage(std::string name, const RenderGraphImageDescription& description);
    RenderGraphResourceHandle createTransientBuffer(std::string name, const RenderGraphBufferDescription& description);

    RenderGraphPassHandle addPass(std::string name, RenderGraphQueue queue, std::span<const RenderGraphAccess> accesses,
                                  std::span<const RenderGraphPassHandle> dependencies = {});
    RenderGraphCompileResult compile(bool conservativeSynchronization = false) const;

    const std::string& resourceName(RenderGraphResourceHandle resource) const;
    const std::string& passName(RenderGraphPassHandle pass) const;

  private:
    struct Resource {
        std::string name;
        RenderGraphResourceKind kind{RenderGraphResourceKind::Image};
        bool imported{false};
        RenderGraphImageDescription image;
        RenderGraphBufferDescription buffer;
        uint32_t initialLayout{0};
        RenderGraphQueue owner{RenderGraphQueue::Graphics};
    };

    struct Pass {
        std::string name;
        RenderGraphQueue queue{RenderGraphQueue::Graphics};
        std::vector<RenderGraphAccess> accesses;
        std::vector<RenderGraphPassHandle> dependencies;
    };

    std::vector<Resource> m_resources;
    std::vector<Pass> m_passes;
};
