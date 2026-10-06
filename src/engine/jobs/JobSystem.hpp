#pragma once

// Bounded prioritized background job system.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

enum class JobStatus : uint8_t {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

struct JobResult {
    bool success{true};
    std::string payload;
    std::shared_ptr<const void> object;
    std::string error;
};

class JobContext {
  public:
    struct State;
    bool cancellationRequested() const;
    void setProgress(float progress);
    size_t workerIndex() const;
    bool mainThread() const;

  private:
    friend class JobSystem;
    explicit JobContext(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
};

class JobHandle {
  public:
    JobHandle() = default;
    uint64_t id() const;
    JobStatus status() const;
    float progress() const;
    void cancel() const;
    explicit operator bool() const;

  private:
    friend class JobSystem;
    explicit JobHandle(std::shared_ptr<JobContext::State> state);
    std::shared_ptr<JobContext::State> m_state;
};

struct CompletedJob {
    JobHandle handle;
    JobResult result;
};

struct JobSystemConfig {
    size_t workerCount{0};
    size_t queueCapacity{64};
    bool deterministic{false};
};

class JobSystem {
  public:
    using Function = std::function<JobResult(JobContext&)>;

    explicit JobSystem(JobSystemConfig config = {});
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    JobHandle submit(Function function, int priority = 0);
    bool reprioritize(const JobHandle& handle, int priority);
    bool pumpOne();
    std::vector<CompletedJob> takeCompleted();
    std::vector<CompletedJob> takeCompleted(std::span<const uint64_t> jobIds);
    std::optional<CompletedJob> waitCompleted(std::span<const uint64_t> jobIds);
    void shutdown(bool cancelPending = true);
    size_t pendingCount() const;

  private:
    struct QueuedJob {
        int priority{0};
        uint64_t sequence{0};
        Function function;
        std::shared_ptr<JobContext::State> state;
    };

    bool takeNext(QueuedJob& job);
    void execute(QueuedJob job, size_t workerIndex, bool mainThread);
    void workerLoop(size_t workerIndex);

    JobSystemConfig m_config;
    mutable std::mutex m_mutex;
    std::condition_variable m_available;
    std::condition_variable m_completedAvailable;
    std::deque<QueuedJob> m_queue;
    std::vector<CompletedJob> m_completed;
    std::vector<std::thread> m_workers;
    uint64_t m_nextId{1};
    uint64_t m_nextSequence{1};
    bool m_stopping{false};
};
