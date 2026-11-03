// Job system: priority and submission ordering (also after reprioritize),
// bounded submission, filtered completions, exceptions reported as failed
// jobs, cancellation before and during execution, and shutdown or
// destruction while jobs are running and queued.

#include "engine/jobs/JobSystem.hpp"

#include "support/Doctest.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// Polls a condition another thread makes true; the deadline only bounds a
// broken job system and is never expected to elapse.
bool waitUntil(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

JobSystem::Function recordOrder(std::vector<int>& order, int value) {
    return [&order, value](JobContext&) {
        order.push_back(value);
        return JobResult{};
    };
}

JobSystem::Function blockUntil(std::latch& release) {
    return [&release](JobContext&) {
        release.wait();
        return JobResult{.payload = "blocking job"};
    };
}

const CompletedJob* findCompleted(const std::vector<CompletedJob>& completed, const JobHandle& handle) {
    for (const CompletedJob& job : completed) {
        if (job.handle.id() == handle.id()) {
            return &job;
        }
    }
    return nullptr;
}

} // namespace

TEST_SUITE_BEGIN("JobSystem");

TEST_CASE("JobSystem: higher priorities run first and equal priorities run in submission order") {
    JobSystem jobs({.deterministic = true});
    std::vector<int> order;
    jobs.submit(recordOrder(order, 1), 0);
    jobs.submit(recordOrder(order, 2), 5);
    jobs.submit(recordOrder(order, 3), 0);
    jobs.submit(recordOrder(order, 4), 5);
    jobs.submit(recordOrder(order, 5), -1);
    CHECK(jobs.pendingCount() == 5);

    while (jobs.pumpOne()) {
    }

    CHECK((order == std::vector<int>{2, 4, 1, 3, 5}));
    CHECK(jobs.pendingCount() == 0);
    CHECK_FALSE(jobs.pumpOne());
}

TEST_CASE("JobSystem: a reprioritized job keeps its submission order within its new priority") {
    JobSystem jobs({.deterministic = true});
    std::vector<int> order;
    const JobHandle older = jobs.submit(recordOrder(order, 1), 3);
    const JobHandle newer = jobs.submit(recordOrder(order, 2), 5);
    jobs.submit(recordOrder(order, 3), 3);

    CHECK(jobs.reprioritize(newer, 3));
    while (jobs.pumpOne()) {
    }

    CHECK_MESSAGE((order == std::vector<int>{1, 2, 3}), "the lowered job does not overtake older equal-priority work");
    CHECK_MESSAGE(!jobs.reprioritize(older, 9), "a job that has already run cannot be reprioritized");
    CHECK_MESSAGE(!jobs.reprioritize(JobHandle{}, 9), "an empty handle names no queued job");
}

TEST_CASE("JobSystem: submission is refused for an empty function or a full queue") {
    JobSystem jobs({.queueCapacity = 2, .deterministic = true});
    std::vector<int> order;

    CHECK_FALSE(jobs.submit(JobSystem::Function{}));
    const JobHandle first = jobs.submit(recordOrder(order, 1));
    const JobHandle second = jobs.submit(recordOrder(order, 2));
    const JobHandle overflow = jobs.submit(recordOrder(order, 3));

    CHECK(first);
    CHECK(second);
    CHECK(first.id() != second.id());
    CHECK_FALSE(overflow);
    CHECK(overflow.id() == 0);
    CHECK(jobs.pendingCount() == 2);

    jobs.pumpOne();
    CHECK_MESSAGE(jobs.submit(recordOrder(order, 4)), "running a job frees queue capacity");
}

TEST_CASE("JobSystem: completions are taken by job id and carry the job result") {
    JobSystem jobs({.deterministic = true});
    const JobHandle first = jobs.submit([](JobContext& context) {
        CHECK(context.mainThread());
        context.setProgress(0.5f);
        return JobResult{.payload = "first"};
    });
    const JobHandle second = jobs.submit([](JobContext&) { return JobResult{.payload = "second"}; });
    CHECK(first.status() == JobStatus::Queued);
    jobs.pumpOne();
    jobs.pumpOne();

    const std::array<uint64_t, 1> secondId{second.id()};
    const std::vector<CompletedJob> taken = jobs.takeCompleted(secondId);
    REQUIRE(taken.size() == 1);
    CHECK(taken.front().result.payload == "second");

    const std::array<uint64_t, 1> firstId{first.id()};
    const std::optional<CompletedJob> waited = jobs.waitCompleted(firstId);
    REQUIRE(waited.has_value());
    CHECK(waited->result.payload == "first");
    CHECK(first.status() == JobStatus::Succeeded);
    CHECK_MESSAGE(first.progress() == doctest::Approx(1.0f), "a finished job reports full progress");
    CHECK(jobs.takeCompleted().empty());
}

TEST_CASE("JobSystem: a throwing job completes as failed and the worker keeps running jobs") {
    JobSystem jobs({.workerCount = 1});
    const JobHandle throwsStandard =
        jobs.submit([](JobContext&) -> JobResult { throw std::runtime_error("decoder exploded"); });
    const JobHandle throwsOther = jobs.submit([](JobContext&) -> JobResult { throw 42; });
    const JobHandle fails = jobs.submit([](JobContext&) { return JobResult{.success = false, .error = "bad"}; });
    const JobHandle after = jobs.submit([](JobContext& context) {
        CHECK_FALSE(context.mainThread());
        return JobResult{.payload = "still running"};
    });

    const std::array<uint64_t, 1> afterId{after.id()};
    const std::optional<CompletedJob> last = jobs.waitCompleted(afterId);
    REQUIRE(last.has_value());
    CHECK(last->result.success);
    CHECK(last->result.payload == "still running");

    const std::vector<CompletedJob> completed = jobs.takeCompleted();
    const CompletedJob* standard = findCompleted(completed, throwsStandard);
    const CompletedJob* other = findCompleted(completed, throwsOther);
    const CompletedJob* reported = findCompleted(completed, fails);
    REQUIRE(standard != nullptr);
    REQUIRE(other != nullptr);
    REQUIRE(reported != nullptr);
    CHECK(throwsStandard.status() == JobStatus::Failed);
    CHECK_FALSE(standard->result.success);
    CHECK(standard->result.error == "decoder exploded");
    CHECK(throwsOther.status() == JobStatus::Failed);
    CHECK(other->result.error == "Job failed with an unknown exception");
    CHECK(fails.status() == JobStatus::Failed);
    CHECK(reported->result.error == "bad");
}

TEST_CASE("JobSystem: a job cancelled before it runs completes as cancelled without running") {
    JobSystem jobs({.deterministic = true});
    bool ran = false;
    const JobHandle handle = jobs.submit([&ran](JobContext&) {
        ran = true;
        return JobResult{};
    });

    handle.cancel();
    jobs.pumpOne();

    CHECK_FALSE(ran);
    CHECK(handle.status() == JobStatus::Cancelled);
    const std::vector<CompletedJob> completed = jobs.takeCompleted();
    REQUIRE(completed.size() == 1);
    CHECK_FALSE(completed.front().result.success);
    CHECK(completed.front().result.error == "Job cancelled before execution");
}

TEST_CASE("JobSystem: a running job observes cancellation and completes as cancelled") {
    JobSystem jobs({.workerCount = 1});
    const JobHandle handle = jobs.submit([](JobContext& context) {
        const bool observed = waitUntil([&context] { return context.cancellationRequested(); });
        return JobResult{.payload = observed ? "observed" : "timed out"};
    });

    REQUIRE(waitUntil([&handle] { return handle.status() == JobStatus::Running; }));
    handle.cancel();
    const std::array<uint64_t, 1> id{handle.id()};
    const std::optional<CompletedJob> completed = jobs.waitCompleted(id);

    REQUIRE(completed.has_value());
    CHECK(completed->result.payload == "observed");
    CHECK_FALSE(completed->result.success);
    CHECK(completed->result.error == "Job cancelled");
    CHECK(handle.status() == JobStatus::Cancelled);
}

TEST_CASE("JobSystem: shutdown while busy cancels queued jobs and waits for the running one") {
    JobSystem jobs({.workerCount = 1});
    std::latch release(1);
    const JobHandle running = jobs.submit(blockUntil(release));
    const bool started = waitUntil([&running] { return running.status() == JobStatus::Running; });
    const JobHandle queuedFirst = jobs.submit([](JobContext&) { return JobResult{}; });
    const JobHandle queuedSecond = jobs.submit([](JobContext&) { return JobResult{}; });

    std::thread stopper([&jobs] { jobs.shutdown(true); });
    const bool queuedCancelled = waitUntil(
        [&] { return queuedFirst.status() == JobStatus::Cancelled && queuedSecond.status() == JobStatus::Cancelled; });
    const JobStatus runningDuringShutdown = running.status();
    const bool refusedDuringShutdown = !jobs.submit([](JobContext&) { return JobResult{}; });
    release.count_down();
    stopper.join();

    CHECK(started);
    CHECK(queuedCancelled);
    CHECK_MESSAGE(runningDuringShutdown == JobStatus::Running, "shutdown does not interrupt a running job");
    CHECK(refusedDuringShutdown);
    CHECK(running.status() == JobStatus::Succeeded);
    CHECK(jobs.pendingCount() == 0);
    const std::vector<CompletedJob> completed = jobs.takeCompleted();
    CHECK(completed.size() == 3);
    const CompletedJob* finished = findCompleted(completed, running);
    const CompletedJob* cancelled = findCompleted(completed, queuedFirst);
    REQUIRE(finished != nullptr);
    REQUIRE(cancelled != nullptr);
    CHECK(finished->result.payload == "blocking job");
    CHECK(cancelled->result.error == "Job cancelled at shutdown");
}

TEST_CASE("JobSystem: shutdown without cancellation drains queued jobs before returning") {
    JobSystem jobs({.workerCount = 1});
    std::latch release(1);
    const JobHandle running = jobs.submit(blockUntil(release));
    const bool started = waitUntil([&running] { return running.status() == JobStatus::Running; });
    std::vector<JobHandle> queued;
    for (int index = 0; index < 4; ++index) {
        queued.push_back(jobs.submit([](JobContext&) { return JobResult{.payload = "drained"}; }));
    }

    std::thread stopper([&jobs] { jobs.shutdown(false); });
    release.count_down();
    stopper.join();

    CHECK(started);
    CHECK(running.status() == JobStatus::Succeeded);
    for (const JobHandle& handle : queued) {
        CHECK(handle.status() == JobStatus::Succeeded);
    }
    CHECK(jobs.pendingCount() == 0);
    CHECK(jobs.takeCompleted().size() == 5);
    CHECK_FALSE(jobs.submit([](JobContext&) { return JobResult{}; }));
}

TEST_CASE("JobSystem: destruction with queued work finishes or cancels every job") {
    std::vector<JobHandle> handles;
    std::atomic<int> executed{0};
    {
        JobSystem jobs({.workerCount = 2, .queueCapacity = 64});
        for (int index = 0; index < 64; ++index) {
            handles.push_back(jobs.submit([&executed](JobContext&) {
                executed.fetch_add(1);
                return JobResult{};
            }));
        }
    }

    int succeeded = 0;
    int cancelled = 0;
    for (const JobHandle& handle : handles) {
        REQUIRE(handle);
        const JobStatus status = handle.status();
        CHECK_MESSAGE((status == JobStatus::Succeeded || status == JobStatus::Cancelled),
                      "no job is left queued or running after destruction");
        succeeded += status == JobStatus::Succeeded ? 1 : 0;
        cancelled += status == JobStatus::Cancelled ? 1 : 0;
    }
    CHECK(succeeded == executed.load());
    CHECK(succeeded + cancelled == 64);
}

TEST_CASE("JobSystem: waiting after shutdown returns instead of blocking") {
    JobSystem jobs({.deterministic = true});
    const JobHandle queued = jobs.submit([](JobContext&) { return JobResult{}; });
    jobs.shutdown();
    jobs.shutdown();

    const std::array<uint64_t, 1> unknownId{queued.id() + 100};
    CHECK_FALSE(jobs.waitCompleted(unknownId).has_value());
    const std::array<uint64_t, 1> queuedId{queued.id()};
    const std::optional<CompletedJob> cancelled = jobs.waitCompleted(queuedId);
    REQUIRE(cancelled.has_value());
    CHECK(cancelled->result.error == "Job cancelled at shutdown");
    CHECK(queued.status() == JobStatus::Cancelled);
}

TEST_SUITE_END();
