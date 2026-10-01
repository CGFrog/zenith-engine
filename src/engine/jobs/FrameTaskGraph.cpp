#include "FrameTaskGraph.hpp"

#include "JobSystem.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>

bool FrameTaskContext::cancellationRequested() const {
    return cancelled != nullptr && cancelled->load(std::memory_order_acquire);
}

FrameTaskGraph::FrameTaskGraph(uint64_t generation, FrameTaskGraphConfig config)
    : m_generation(generation)
    , m_config(config) {
    m_config.minimumGrainSize = std::max<size_t>(m_config.minimumGrainSize, 1);
    if (m_config.workerCount == 0) {
        m_config.workerCount = std::max<size_t>(std::thread::hardware_concurrency(), 1);
    }
    if (m_config.deterministic) {
        m_config.workerCount = 1;
    }
}

FrameTaskId FrameTaskGraph::add(std::string name, std::span<const FrameResourceUse> resources, Function function,
                                size_t itemCount, std::span<const FrameTaskId> dependencies) {
    Node node{.name = std::move(name),
              .resources = {resources.begin(), resources.end()},
              .function = std::move(function),
              .itemCount = itemCount,
              .dependencies = {dependencies.begin(), dependencies.end()}};
    for (FrameTaskId earlier = 0; earlier < m_nodes.size(); ++earlier) {
        if (conflicts(m_nodes[earlier], node) &&
            std::ranges::find(node.dependencies, earlier) == node.dependencies.end()) {
            node.dependencies.push_back(earlier);
        }
    }
    m_nodes.push_back(std::move(node));
    return static_cast<FrameTaskId>(m_nodes.size() - 1);
}

void FrameTaskGraph::cancel() {
    m_cancelled.store(true, std::memory_order_release);
}

uint64_t FrameTaskGraph::generation() const {
    return m_generation;
}

bool FrameTaskGraph::conflicts(const Node& earlier, const Node& later) const {
    for (const FrameResourceUse& first : earlier.resources) {
        for (const FrameResourceUse& second : later.resources) {
            if (first.name == second.name &&
                (first.access == FrameResourceAccess::Write || second.access == FrameResourceAccess::Write)) {
                return true;
            }
        }
    }
    return false;
}

class FrameTaskExecutor::Impl {
  public:
    explicit Impl(FrameTaskGraphConfig value)
        : config(value)
        , jobs({.workerCount = normalizedWorkerCount(value),
                .queueCapacity = 65536,
                .deterministic = value.deterministic}) {
        config.workerCount = normalizedWorkerCount(value);
        config.minimumGrainSize = std::max<size_t>(config.minimumGrainSize, 1);
    }

    static size_t normalizedWorkerCount(const FrameTaskGraphConfig& config) {
        if (config.deterministic) {
            return 1;
        }
        return config.workerCount == 0 ? std::max<size_t>(std::thread::hardware_concurrency(), 1) : config.workerCount;
    }

    FrameTaskGraphConfig config;
    JobSystem jobs;
    std::mutex executionMutex;
    std::mutex activeMutex;
    FrameTaskGraph* activeGraph{nullptr};
};

FrameTaskExecutor::FrameTaskExecutor(FrameTaskGraphConfig config)
    : m_impl(std::make_unique<Impl>(config)) {}

FrameTaskExecutor::~FrameTaskExecutor() = default;

void FrameTaskExecutor::cancelActive() {
    std::scoped_lock lock(m_impl->activeMutex);
    if (m_impl->activeGraph != nullptr) {
        m_impl->activeGraph->cancel();
    }
}

size_t FrameTaskExecutor::workerCount() const {
    return m_impl->config.workerCount;
}

FrameTaskResult FrameTaskGraph::execute() {
    FrameTaskExecutor executor(m_config);
    return executor.execute(*this);
}

FrameTaskResult FrameTaskExecutor::execute(FrameTaskGraph& graph) {
    std::unique_lock executionLock(m_impl->executionMutex);
    {
        std::scoped_lock lock(m_impl->activeMutex);
        m_impl->activeGraph = &graph;
    }
    const auto begin = std::chrono::steady_clock::now();
    FrameTaskResult result{.stats = {.nodeCount = static_cast<uint32_t>(graph.m_nodes.size())}};
    const auto finish = [&] {
        result.stats.latencyMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        {
            std::scoped_lock lock(m_impl->activeMutex);
            m_impl->activeGraph = nullptr;
        }
        return result;
    };
    if (graph.m_cancelled.load(std::memory_order_acquire)) {
        result.cancelled = true;
        result.error = "Frame task graph was cancelled before execution";
        return finish();
    }
    for (const FrameTaskGraph::Node& node : graph.m_nodes) {
        if (!node.function) {
            result.error = "Frame task '" + node.name + "' has no function";
            return finish();
        }
        for (FrameTaskId dependency : node.dependencies) {
            if (dependency >= graph.m_nodes.size()) {
                result.error = "Frame task '" + node.name + "' has an invalid dependency";
                return finish();
            }
        }
    }

    std::vector<uint32_t> remainingDependencies(graph.m_nodes.size());
    std::vector<std::vector<FrameTaskId>> dependents(graph.m_nodes.size());
    for (FrameTaskId id = 0; id < graph.m_nodes.size(); ++id) {
        remainingDependencies[id] = static_cast<uint32_t>(graph.m_nodes[id].dependencies.size());
        for (FrameTaskId dependency : graph.m_nodes[id].dependencies) {
            dependents[dependency].push_back(id);
        }
    }

    std::vector<uint32_t> validationDependencies = remainingDependencies;
    std::deque<FrameTaskId> validationQueue;
    for (FrameTaskId id = 0; id < graph.m_nodes.size(); ++id) {
        if (validationDependencies[id] == 0) {
            validationQueue.push_back(id);
        }
    }
    size_t validatedNodes = 0;
    while (!validationQueue.empty()) {
        const FrameTaskId completed = validationQueue.front();
        validationQueue.pop_front();
        ++validatedNodes;
        for (FrameTaskId dependent : dependents[completed]) {
            if (--validationDependencies[dependent] == 0) {
                validationQueue.push_back(dependent);
            }
        }
    }
    if (validatedNodes != graph.m_nodes.size()) {
        result.error = "Frame task graph contains a dependency cycle";
        return finish();
    }

    struct Work {
        FrameTaskId node{0};
        size_t begin{0};
        size_t end{0};
    };

    std::vector<uint32_t> remainingRanges(graph.m_nodes.size());
    std::vector<uint64_t> activeJobs;
    activeJobs.reserve(1024);
    std::unordered_map<uint64_t, Work> workByJob;
    workByJob.reserve(1024);
    std::atomic_uint64_t busyNanoseconds{0};
    bool submitFailed = false;
    const auto enqueueNode = [&](FrameTaskId id) {
        const size_t count = graph.m_nodes[id].itemCount;
        const size_t grain = std::max<size_t>(graph.m_config.minimumGrainSize, 1);
        const size_t rangeCount = count == 0 ? 1 : (count + grain - 1) / grain;
        remainingRanges[id] = static_cast<uint32_t>(rangeCount);
        for (size_t range = 0; range < rangeCount; ++range) {
            const Work work{id, count == 0 ? 0 : range * grain,
                            count == 0 ? 0 : std::min(count, range * grain + grain)};
            JobHandle handle = m_impl->jobs.submit([&graph, work, &busyNanoseconds](JobContext& jobContext) {
                if (graph.m_cancelled.load(std::memory_order_acquire) || jobContext.cancellationRequested()) {
                    return JobResult{.success = false, .error = "Frame task cancelled"};
                }
                const auto workBegin = std::chrono::steady_clock::now();
                graph.m_nodes[work.node].function(
                    {graph.m_generation, work.begin, work.end, jobContext.workerIndex(), &graph.m_cancelled});
                busyNanoseconds.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                    std::chrono::steady_clock::now() - workBegin)
                                                                    .count()),
                                          std::memory_order_relaxed);
                return JobResult{};
            });
            if (!handle) {
                submitFailed = true;
                graph.m_cancelled.store(true, std::memory_order_release);
                return;
            }
            activeJobs.push_back(handle.id());
            workByJob.emplace(handle.id(), work);
        }
        result.stats.jobCount += static_cast<uint32_t>(rangeCount);
        result.stats.maximumQueueDepth =
            std::max(result.stats.maximumQueueDepth, static_cast<uint32_t>(m_impl->jobs.pendingCount()));
    };
    for (FrameTaskId id = 0; id < graph.m_nodes.size(); ++id) {
        if (remainingDependencies[id] == 0) {
            enqueueNode(id);
        }
    }
    size_t completedNodes = 0;
    if (graph.m_nodes.empty()) {
        result.success = true;
    } else {
        while (!activeJobs.empty() ||
               (!submitFailed && result.error.empty() && completedNodes != graph.m_nodes.size())) {
            const auto assistBegin = std::chrono::steady_clock::now();
            if (m_impl->jobs.pumpOne()) {
                result.stats.mainThreadAssistMilliseconds +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - assistBegin).count();
                ++result.stats.stolenJobs;
            }
            std::vector<CompletedJob> completed = m_impl->jobs.takeCompleted(activeJobs);
            if (completed.empty() && !activeJobs.empty()) {
                if (std::optional<CompletedJob> waited = m_impl->jobs.waitCompleted(activeJobs)) {
                    completed.push_back(std::move(*waited));
                }
            }
            for (CompletedJob& job : completed) {
                const uint64_t id = job.handle.id();
                const auto metadata = workByJob.find(id);
                if (metadata == workByJob.end()) {
                    continue;
                }
                const Work work = metadata->second;
                workByJob.erase(metadata);
                std::erase(activeJobs, id);
                if (!job.result.success) {
                    if (result.error.empty()) {
                        result.error = job.result.error;
                    }
                    graph.m_cancelled.store(true, std::memory_order_release);
                    continue;
                }
                if (--remainingRanges[work.node] == 0) {
                    ++completedNodes;
                    for (FrameTaskId dependent : dependents[work.node]) {
                        if (--remainingDependencies[dependent] == 0) {
                            enqueueNode(dependent);
                        }
                    }
                }
            }
        }
        if (submitFailed) {
            result.error = "Frame task executor queue capacity was exceeded";
        } else if (graph.m_cancelled.load(std::memory_order_acquire)) {
            result.cancelled = true;
            if (result.error.empty()) {
                result.error = "Frame task graph was cancelled";
            }
        } else if (completedNodes != graph.m_nodes.size()) {
            result.error = "Frame task graph contains a dependency cycle";
        } else {
            result.success = true;
        }
        const double elapsedNanoseconds = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        const size_t participatingThreads = m_impl->config.deterministic ? 1 : m_impl->config.workerCount + 1;
        if (elapsedNanoseconds > 0.0) {
            result.stats.workerUtilization =
                std::min(1.0, static_cast<double>(busyNanoseconds.load(std::memory_order_relaxed)) /
                                  (elapsedNanoseconds * static_cast<double>(participatingThreads)));
        }
    }
    return finish();
}
