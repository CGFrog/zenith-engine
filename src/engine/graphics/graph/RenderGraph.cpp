#include "RenderGraph.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_set>

namespace {
constexpr uint32_t invalidIndex = UINT32_MAX;

bool writes(RenderGraphAccessMode mode) {
    return mode != RenderGraphAccessMode::Read;
}

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    const uint64_t safeAlignment = std::max<uint64_t>(alignment, 1);
    return (value + safeAlignment - 1) / safeAlignment * safeAlignment;
}
} // namespace

RenderGraphResourceHandle RenderGraph::importImage(std::string name, const RenderGraphImageDescription& description,
                                                   uint32_t initialLayout, RenderGraphQueue owner) {
    const RenderGraphResourceHandle handle{static_cast<uint32_t>(m_resources.size())};
    m_resources.push_back({.name = std::move(name),
                           .kind = RenderGraphResourceKind::Image,
                           .imported = true,
                           .image = description,
                           .initialLayout = initialLayout,
                           .owner = owner});
    return handle;
}

RenderGraphResourceHandle RenderGraph::importBuffer(std::string name, const RenderGraphBufferDescription& description,
                                                    RenderGraphQueue owner) {
    const RenderGraphResourceHandle handle{static_cast<uint32_t>(m_resources.size())};
    m_resources.push_back({.name = std::move(name),
                           .kind = RenderGraphResourceKind::Buffer,
                           .imported = true,
                           .buffer = description,
                           .owner = owner});
    return handle;
}

RenderGraphResourceHandle RenderGraph::createTransientImage(std::string name,
                                                            const RenderGraphImageDescription& description) {
    const RenderGraphResourceHandle handle{static_cast<uint32_t>(m_resources.size())};
    m_resources.push_back({.name = std::move(name), .kind = RenderGraphResourceKind::Image, .image = description});
    return handle;
}

RenderGraphResourceHandle RenderGraph::createTransientBuffer(std::string name,
                                                             const RenderGraphBufferDescription& description) {
    const RenderGraphResourceHandle handle{static_cast<uint32_t>(m_resources.size())};
    m_resources.push_back({.name = std::move(name), .kind = RenderGraphResourceKind::Buffer, .buffer = description});
    return handle;
}

RenderGraphPassHandle RenderGraph::addPass(std::string name, RenderGraphQueue queue,
                                           std::span<const RenderGraphAccess> accesses,
                                           std::span<const RenderGraphPassHandle> dependencies) {
    const RenderGraphPassHandle handle{static_cast<uint32_t>(m_passes.size())};
    m_passes.push_back({.name = std::move(name),
                        .queue = queue,
                        .accesses = {accesses.begin(), accesses.end()},
                        .dependencies = {dependencies.begin(), dependencies.end()}});
    return handle;
}

RenderGraphCompileResult RenderGraph::compile(bool conservativeSynchronization) const {
    RenderGraphCompileResult result;
    const uint32_t passCount = static_cast<uint32_t>(m_passes.size());
    const uint32_t resourceCount = static_cast<uint32_t>(m_resources.size());
    std::vector<std::unordered_set<uint32_t>> edges(passCount);
    std::vector<uint32_t> indegree(passCount, 0);
    std::vector<uint32_t> lastWriter(resourceCount, invalidIndex);
    std::vector<std::vector<uint32_t>> readers(resourceCount);

    const auto addEdge = [&](uint32_t source, uint32_t destination) {
        if (source != destination && edges[source].insert(destination).second) {
            ++indegree[destination];
        }
    };

    for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex) {
        const Pass& pass = m_passes[passIndex];
        std::unordered_set<uint32_t> usedResources;
        for (RenderGraphPassHandle dependency : pass.dependencies) {
            if (!dependency || dependency.index >= passCount) {
                result.error = "Render pass '" + pass.name + "' has an invalid explicit dependency";
                return result;
            }
            addEdge(dependency.index, passIndex);
        }
        for (const RenderGraphAccess& access : pass.accesses) {
            if (!access.resource || access.resource.index >= resourceCount) {
                result.error = "Render pass '" + pass.name + "' references an invalid resource";
                return result;
            }
            if (!usedResources.insert(access.resource.index).second) {
                result.error = "Render pass '" + pass.name + "' declares a resource more than once";
                return result;
            }
            const uint32_t resource = access.resource.index;
            if (lastWriter[resource] != invalidIndex) {
                addEdge(lastWriter[resource], passIndex);
            }
            if (writes(access.mode)) {
                for (uint32_t reader : readers[resource]) {
                    addEdge(reader, passIndex);
                }
                readers[resource].clear();
                lastWriter[resource] = passIndex;
            } else {
                readers[resource].push_back(passIndex);
            }
        }
    }

    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
    for (uint32_t i = 0; i < passCount; ++i) {
        if (indegree[i] == 0) {
            ready.push(i);
        }
    }
    while (!ready.empty()) {
        const uint32_t pass = ready.top();
        ready.pop();
        result.graph.executionOrder.push_back({pass});
        for (uint32_t successor : edges[pass]) {
            if (--indegree[successor] == 0) {
                ready.push(successor);
            }
        }
    }
    if (result.graph.executionOrder.size() != passCount) {
        result.error = "Render graph contains a dependency cycle";
        return result;
    }

    struct State {
        bool used{false};
        RenderGraphPassHandle pass;
        RenderGraphQueue queue{RenderGraphQueue::Graphics};
        RenderGraphAccess access;
    };

    std::vector<State> states(resourceCount);
    for (uint32_t i = 0; i < resourceCount; ++i) {
        states[i].queue = m_resources[i].owner;
        states[i].access.layout = m_resources[i].initialLayout;
    }
    for (RenderGraphPassHandle passHandle : result.graph.executionOrder) {
        const Pass& pass = m_passes[passHandle.index];
        for (const RenderGraphAccess& access : pass.accesses) {
            State& previous = states[access.resource.index];
            const bool layoutChange = m_resources[access.resource.index].kind == RenderGraphResourceKind::Image &&
                                      previous.access.layout != access.layout;
            const bool queueChange = previous.queue != pass.queue;
            const bool hazard = previous.used && (writes(previous.access.mode) || writes(access.mode));
            if (layoutChange || queueChange || hazard || (conservativeSynchronization && previous.used)) {
                result.graph.barriers.push_back({.resource = access.resource,
                                                 .sourcePass = previous.pass,
                                                 .destinationPass = passHandle,
                                                 .sourceQueue = previous.queue,
                                                 .destinationQueue = pass.queue,
                                                 .sourceStages = previous.used ? previous.access.stages : 0,
                                                 .destinationStages = access.stages,
                                                 .sourceAccess = previous.used ? previous.access.access : 0,
                                                 .destinationAccess = access.access,
                                                 .oldLayout = previous.access.layout,
                                                 .newLayout = access.layout,
                                                 .queueOwnershipTransfer = queueChange});
            }
            previous = {.used = true, .pass = passHandle, .queue = pass.queue, .access = access};
        }
    }

    struct Lifetime {
        uint32_t first{invalidIndex};
        uint32_t last{0};
    };

    std::vector<Lifetime> lifetimes(resourceCount);
    for (uint32_t order = 0; order < passCount; ++order) {
        const Pass& pass = m_passes[result.graph.executionOrder[order].index];
        for (const RenderGraphAccess& access : pass.accesses) {
            Lifetime& lifetime = lifetimes[access.resource.index];
            lifetime.first = std::min(lifetime.first, order);
            lifetime.last = std::max(lifetime.last, order);
        }
    }

    struct Slot {
        uint32_t resource{invalidIndex};
        uint32_t lastPass{0};
        uint64_t size{0};
    };

    std::vector<Slot> slots;
    const auto compatible = [&](uint32_t first, uint32_t second) {
        const Resource& a = m_resources[first];
        const Resource& b = m_resources[second];
        if (a.kind != b.kind) {
            return false;
        }
        if (a.kind == RenderGraphResourceKind::Image) {
            return a.image.format == b.image.format && a.image.usage == b.image.usage &&
                   a.image.alignment == b.image.alignment && a.image.memoryTypeBits == b.image.memoryTypeBits &&
                   a.image.samples == b.image.samples;
        }
        return a.buffer.usage == b.buffer.usage && a.buffer.alignment == b.buffer.alignment &&
               a.buffer.memoryTypeBits == b.buffer.memoryTypeBits;
    };

    for (uint32_t resource = 0; resource < resourceCount; ++resource) {
        if (m_resources[resource].imported || lifetimes[resource].first == invalidIndex) {
            continue;
        }
        const Resource& description = m_resources[resource];
        const uint64_t requested = description.kind == RenderGraphResourceKind::Image ? description.image.requestedBytes
                                                                                      : description.buffer.size;
        const uint64_t alignment = description.kind == RenderGraphResourceKind::Image ? description.image.alignment
                                                                                      : description.buffer.alignment;
        const uint64_t allocationSize = alignUp(requested, alignment);
        result.graph.statistics.requestedTransientBytes += allocationSize;
        uint32_t slotIndex = invalidIndex;
        for (uint32_t i = 0; i < slots.size(); ++i) {
            if (slots[i].lastPass < lifetimes[resource].first && compatible(slots[i].resource, resource)) {
                slotIndex = i;
                break;
            }
        }
        const bool aliased = slotIndex != invalidIndex;
        if (!aliased) {
            slotIndex = static_cast<uint32_t>(slots.size());
            slots.push_back({.resource = resource, .lastPass = lifetimes[resource].last, .size = allocationSize});
        } else {
            Slot& slot = slots[slotIndex];
            slot.resource = resource;
            slot.lastPass = lifetimes[resource].last;
            slot.size = std::max(slot.size, allocationSize);
        }
        result.graph.allocations.push_back({.resource = {resource},
                                            .slot = slotIndex,
                                            .size = allocationSize,
                                            .firstPass = lifetimes[resource].first,
                                            .lastPass = lifetimes[resource].last,
                                            .aliased = aliased});
    }

    for (const Slot& slot : slots) {
        result.graph.statistics.committedTransientBytes += slot.size;
    }
    result.graph.statistics.aliasedTransientBytes =
        result.graph.statistics.requestedTransientBytes - result.graph.statistics.committedTransientBytes;
    for (uint32_t pass = 0; pass < passCount; ++pass) {
        uint64_t activeBytes = 0;
        std::unordered_set<uint32_t> activeSlots;
        for (const RenderGraphAllocation& allocation : result.graph.allocations) {
            if (allocation.firstPass <= pass && allocation.lastPass >= pass &&
                activeSlots.insert(allocation.slot).second) {
                activeBytes += slots[allocation.slot].size;
            }
        }
        result.graph.statistics.peakTransientBytes = std::max(result.graph.statistics.peakTransientBytes, activeBytes);
    }
    result.graph.statistics.passCount = passCount;
    result.graph.statistics.barrierCount = static_cast<uint32_t>(result.graph.barriers.size());
    result.graph.statistics.queueTransferCount = static_cast<uint32_t>(std::ranges::count_if(
        result.graph.barriers, [](const RenderGraphBarrier& barrier) { return barrier.queueOwnershipTransfer; }));
    result.success = true;
    return result;
}

const std::string& RenderGraph::resourceName(RenderGraphResourceHandle resource) const {
    return m_resources.at(resource.index).name;
}

const std::string& RenderGraph::passName(RenderGraphPassHandle pass) const {
    return m_passes.at(pass.index).name;
}

bool shouldEnableAsyncCompute(const AsyncComputeMeasurements& measurements) {
    if (!measurements.separateComputeQueue || measurements.serialGpuMilliseconds <= 0.0 ||
        measurements.asynchronousGpuMilliseconds <= 0.0 || measurements.measuredOverlapMilliseconds <= 0.0) {
        return false;
    }
    return measurements.serialGpuMilliseconds - measurements.asynchronousGpuMilliseconds >=
           measurements.minimumRequiredGainMilliseconds;
}
