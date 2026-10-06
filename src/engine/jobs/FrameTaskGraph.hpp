#pragma once

// Per-frame parallel task graph with declared resource access.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

using FrameTaskId = uint32_t;

enum class FrameResourceAccess : uint8_t {
    Read,
    Write,
};

struct FrameResourceUse {
    std::string name;
    FrameResourceAccess access{FrameResourceAccess::Read};
};

struct FrameTaskContext {
    uint64_t generation{0};
    size_t rangeBegin{0};
    size_t rangeEnd{0};
    size_t workerIndex{0};
    const std::atomic_bool* cancelled{nullptr};

    bool cancellationRequested() const;
};

struct FrameTaskGraphConfig {
    size_t workerCount{0};
    size_t minimumGrainSize{128};
    bool deterministic{false};
};

struct FrameTaskGraphStats {
    uint32_t nodeCount{0};
    uint32_t jobCount{0};
    uint32_t maximumQueueDepth{0};
    uint32_t stolenJobs{0};
    double workerUtilization{0.0};
    double mainThreadAssistMilliseconds{0.0};
    double latencyMilliseconds{0.0};
};

struct FrameTaskResult {
    bool success{false};
    bool cancelled{false};
    std::string error;
    FrameTaskGraphStats stats;

    explicit operator bool() const {
        return success;
    }
};

class FrameTaskGraph;

class FrameTaskExecutor {
  public:
    explicit FrameTaskExecutor(FrameTaskGraphConfig config = {});
    ~FrameTaskExecutor();
    FrameTaskExecutor(const FrameTaskExecutor&) = delete;
    FrameTaskExecutor& operator=(const FrameTaskExecutor&) = delete;

    FrameTaskResult execute(FrameTaskGraph& graph);
    void cancelActive();
    size_t workerCount() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

class FrameTaskGraph {
  public:
    using Function = std::function<void(const FrameTaskContext&)>;

    explicit FrameTaskGraph(uint64_t generation, FrameTaskGraphConfig config = {});
    FrameTaskId add(std::string name, std::span<const FrameResourceUse> resources, Function function,
                    size_t itemCount = 1, std::span<const FrameTaskId> dependencies = {});
    void cancel();
    FrameTaskResult execute();
    uint64_t generation() const;

  private:
    friend class FrameTaskExecutor;

    struct Node {
        std::string name;
        std::vector<FrameResourceUse> resources;
        Function function;
        size_t itemCount{1};
        std::vector<FrameTaskId> dependencies;
    };

    bool conflicts(const Node& earlier, const Node& later) const;

    uint64_t m_generation{0};
    FrameTaskGraphConfig m_config;
    std::vector<Node> m_nodes;
    std::atomic_bool m_cancelled{false};
};
