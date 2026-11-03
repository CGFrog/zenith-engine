#pragma once

// Bounded prioritized background job system.
//
// Contract (exercised by tests/engine/JobSystemTests.cpp):
// - Jobs are independent: there are no dependencies or continuations. A job
//   that needs another's output waits for its completion (waitCompleted) and
//   submits the follow-up itself.
// - The queue runs the highest priority first and, within one priority, in
//   submission order, also after reprioritize. With several workers this
//   orders the start of jobs, not their completion.
// - Every executed job publishes exactly one CompletedJob. A job that throws
//   completes as Failed with the exception's message as the error; the worker
//   survives and runs later jobs.
// - Deterministic mode starts no workers: jobs run only on the thread that
//   calls pumpOne, so waitCompleted blocks until another thread pumps.

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

    // Returns an empty handle for an empty function, a full queue, or after
    // shutdown.
    JobHandle submit(Function function, int priority = 0);
    // Changes the priority of a queued job; false once it has started.
    bool reprioritize(const JobHandle& handle, int priority);
    bool pumpOne();
    std::vector<CompletedJob> takeCompleted();
    std::vector<CompletedJob> takeCompleted(std::span<const uint64_t> jobIds);
    // Blocks until one of the jobs completes; nullopt once shutdown begins and
    // none of them has completed.
    std::optional<CompletedJob> waitCompleted(std::span<const uint64_t> jobIds);
    // Stops accepting work and joins the workers; idempotent, and the
    // destructor calls it. cancelPending completes every queued job as
    // Cancelled; otherwise the workers drain the queue first (deterministic
    // mode leaves it for pumpOne). Running jobs always finish: shutdown does
    // not request their cancellation, so a job that waits for it must be
    // cancelled through its handle or shutdown blocks.
    void shutdown(bool cancelPending = true);
    size_t pendingCount() const;

  private:
    struct QueuedJob {
        int priority{0};
        uint64_t sequence{0};
        Function function;
        std::shared_ptr<JobContext::State> state;
    };

    void sortQueue();
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
