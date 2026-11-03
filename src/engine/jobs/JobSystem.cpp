#include "JobSystem.hpp"

#include <algorithm>

struct JobContext::State {
    uint64_t id{0};
    std::atomic<JobStatus> status{JobStatus::Queued};
    std::atomic<float> progress{0.0f};
    std::atomic_bool cancel{false};
    size_t workerIndex{0};
    bool mainThread{false};
};

JobContext::JobContext(std::shared_ptr<State> state)
    : m_state(std::move(state)) {}

bool JobContext::cancellationRequested() const {
    return m_state->cancel.load(std::memory_order_relaxed);
}

void JobContext::setProgress(float progress) {
    m_state->progress.store(std::clamp(progress, 0.0f, 1.0f), std::memory_order_relaxed);
}

size_t JobContext::workerIndex() const {
    return m_state->workerIndex;
}

bool JobContext::mainThread() const {
    return m_state->mainThread;
}

JobHandle::JobHandle(std::shared_ptr<JobContext::State> state)
    : m_state(std::move(state)) {}

uint64_t JobHandle::id() const {
    return m_state ? m_state->id : 0;
}

JobStatus JobHandle::status() const {
    return m_state ? m_state->status.load(std::memory_order_acquire) : JobStatus::Cancelled;
}

float JobHandle::progress() const {
    return m_state ? m_state->progress.load(std::memory_order_relaxed) : 0.0f;
}

void JobHandle::cancel() const {
    if (m_state) {
        m_state->cancel.store(true, std::memory_order_release);
    }
}

JobHandle::operator bool() const {
    return static_cast<bool>(m_state);
}

JobSystem::JobSystem(JobSystemConfig config)
    : m_config(config) {
    m_config.queueCapacity = std::max<size_t>(m_config.queueCapacity, 1);
    if (!m_config.deterministic) {
        const size_t hardwareWorkers = std::max(1u, std::thread::hardware_concurrency());
        const size_t workerCount =
            m_config.workerCount == 0 ? std::min<size_t>(hardwareWorkers, 4) : m_config.workerCount;
        for (size_t index = 0; index < workerCount; ++index) {
            m_workers.emplace_back([this, index] { workerLoop(index); });
        }
    }
}

JobSystem::~JobSystem() {
    shutdown();
}

JobHandle JobSystem::submit(Function function, int priority) {
    if (!function) {
        return {};
    }
    std::scoped_lock lock(m_mutex);
    if (m_stopping || m_queue.size() >= m_config.queueCapacity) {
        return {};
    }
    auto state = std::make_shared<JobContext::State>();
    state->id = m_nextId++;
    m_queue.push_back({priority, m_nextSequence++, std::move(function), state});
    sortQueue();
    m_available.notify_one();
    return JobHandle(state);
}

bool JobSystem::reprioritize(const JobHandle& handle, int priority) {
    std::scoped_lock lock(m_mutex);
    const auto found =
        std::ranges::find_if(m_queue, [&](const QueuedJob& job) { return job.state->id == handle.id(); });
    if (found == m_queue.end()) {
        return false;
    }
    found->priority = priority;
    sortQueue();
    return true;
}

void JobSystem::sortQueue() {
    // Highest priority first; the submission sequence keeps equal priorities in
    // submission order, also for a job whose priority changed.
    std::ranges::sort(m_queue, [](const QueuedJob& left, const QueuedJob& right) {
        return left.priority != right.priority ? left.priority > right.priority : left.sequence < right.sequence;
    });
}

bool JobSystem::takeNext(QueuedJob& job) {
    std::scoped_lock lock(m_mutex);
    if (m_queue.empty()) {
        return false;
    }
    job = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

bool JobSystem::pumpOne() {
    QueuedJob job;
    if (!takeNext(job)) {
        return false;
    }
    execute(std::move(job), m_workers.size(), true);
    return true;
}

void JobSystem::execute(QueuedJob job, size_t workerIndex, bool mainThread) {
    JobResult result;
    if (job.state->cancel.load(std::memory_order_acquire)) {
        result.success = false;
        result.error = "Job cancelled before execution";
        job.state->status.store(JobStatus::Cancelled, std::memory_order_release);
    } else {
        job.state->workerIndex = workerIndex;
        job.state->mainThread = mainThread;
        job.state->status.store(JobStatus::Running, std::memory_order_release);
        JobContext context(job.state);
        try {
            result = job.function(context);
        } catch (const std::exception& error) {
            result = {.success = false, .error = error.what()};
        } catch (...) {
            result = {.success = false, .error = "Job failed with an unknown exception"};
        }
        if (job.state->cancel.load(std::memory_order_acquire)) {
            result.success = false;
            result.error = "Job cancelled";
            job.state->status.store(JobStatus::Cancelled, std::memory_order_release);
        } else {
            job.state->progress.store(1.0f, std::memory_order_relaxed);
            job.state->status.store(result.success ? JobStatus::Succeeded : JobStatus::Failed,
                                    std::memory_order_release);
        }
    }
    std::scoped_lock lock(m_mutex);
    m_completed.push_back({JobHandle(job.state), std::move(result)});
    m_completedAvailable.notify_all();
}

std::vector<CompletedJob> JobSystem::takeCompleted() {
    std::scoped_lock lock(m_mutex);
    std::vector<CompletedJob> result;
    result.swap(m_completed);
    return result;
}

std::vector<CompletedJob> JobSystem::takeCompleted(std::span<const uint64_t> jobIds) {
    std::scoped_lock lock(m_mutex);
    std::vector<CompletedJob> result;
    for (auto iterator = m_completed.begin(); iterator != m_completed.end();) {
        if (std::ranges::find(jobIds, iterator->handle.id()) == jobIds.end()) {
            ++iterator;
            continue;
        }
        result.push_back(std::move(*iterator));
        iterator = m_completed.erase(iterator);
    }
    return result;
}

std::optional<CompletedJob> JobSystem::waitCompleted(std::span<const uint64_t> jobIds) {
    std::unique_lock lock(m_mutex);
    m_completedAvailable.wait(lock, [&] {
        return m_stopping || std::ranges::any_of(m_completed, [&](const CompletedJob& job) {
                   return std::ranges::find(jobIds, job.handle.id()) != jobIds.end();
               });
    });
    const auto found = std::ranges::find_if(m_completed, [&](const CompletedJob& job) {
        return std::ranges::find(jobIds, job.handle.id()) != jobIds.end();
    });
    if (found == m_completed.end()) {
        return std::nullopt;
    }
    CompletedJob result = std::move(*found);
    m_completed.erase(found);
    return result;
}

void JobSystem::workerLoop(size_t workerIndex) {
    while (true) {
        QueuedJob job;
        {
            std::unique_lock lock(m_mutex);
            m_available.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping && m_queue.empty()) {
                return;
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        execute(std::move(job), workerIndex, false);
    }
}

void JobSystem::shutdown(bool cancelPending) {
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) {
            return;
        }
        m_stopping = true;
        if (cancelPending) {
            for (QueuedJob& job : m_queue) {
                job.state->cancel.store(true, std::memory_order_release);
                job.state->status.store(JobStatus::Cancelled, std::memory_order_release);
                m_completed.push_back({JobHandle(job.state), {.success = false, .error = "Job cancelled at shutdown"}});
            }
            m_queue.clear();
        }
    }
    m_available.notify_all();
    m_completedAvailable.notify_all();
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_workers.clear();
}

size_t JobSystem::pendingCount() const {
    std::scoped_lock lock(m_mutex);
    return m_queue.size();
}
