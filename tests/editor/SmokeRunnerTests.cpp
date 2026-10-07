// The smoke runner on its own: it only passes the step context through, so
// this test (which compiles SmokeRunner.cpp without the rest of the editor)
// gives the context a definition of its own.

#include "editor/smoke/SmokeRunner.hpp"

#include "support/Doctest.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_BEGIN("SmokeRunner");

struct SmokeStepContext {
    // Each update appends "<step>:<stepFrame>".
    std::vector<std::string> log;
};

namespace {

// Runs for frames frames (Done on the last), or fails on failFrame.
class ScriptedStep final : public SmokeStep {
  public:
    ScriptedStep(std::string name, uint32_t frames, uint32_t failFrame = UINT32_MAX, uint32_t budget = 60)
        : m_name(std::move(name))
        , m_frames(frames)
        , m_failFrame(failFrame)
        , m_budget(budget) {}

    std::string_view name() const override {
        return m_name;
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        context.log.push_back(m_name + ":" + std::to_string(stepFrame));
        if (stepFrame == m_failFrame) {
            return SmokeStepResult::failed("scripted failure");
        }
        return stepFrame + 1 >= m_frames ? SmokeStepResult::done() : SmokeStepResult::running();
    }

    uint32_t frameBudget() const override {
        return m_budget;
    }

  private:
    std::string m_name;
    uint32_t m_frames;
    uint32_t m_failFrame;
    uint32_t m_budget;
};

// Updates the runner until it stops running, at most maxFrames times.
SmokeRunResult run(SmokeRunner& runner, SmokeStepContext& context, int maxFrames = 1000) {
    SmokeRunResult result;
    for (int frame = 0; frame < maxFrames && result.status == SmokeRunStatus::Running; ++frame) {
        result = runner.update(context);
    }
    return result;
}

TEST_CASE("SmokeRunner: empty runner passes") {
    SmokeRunner runner;
    SmokeStepContext context;
    CHECK_MESSAGE(runner.update(context).status == SmokeRunStatus::Passed, "a runner without steps passes at once");
}

TEST_CASE("SmokeRunner: steps run in order with their own frames") {
    SmokeRunner runner;
    runner.add(std::make_unique<ScriptedStep>("a", 2));
    runner.add(std::make_unique<ScriptedStep>("b", 1));
    runner.add(std::make_unique<ScriptedStep>("c", 3));
    SmokeStepContext context;
    const SmokeRunResult result = run(runner, context);
    CHECK_MESSAGE((result.status == SmokeRunStatus::Passed && result.step.empty()),
                  "every step finishing passes the run");
    const std::vector<std::string> expected{"a:0", "a:1", "b:0", "c:0", "c:1", "c:2"};
    CHECK_MESSAGE(context.log == expected,
                  "steps run one at a time in the order added, each counting its frames from 0, one update per frame");
    CHECK_MESSAGE((runner.update(context).status == SmokeRunStatus::Passed && context.log.size() == expected.size()),
                  "a finished run runs nothing more");
}

TEST_CASE("SmokeRunner: failure stops the run and names the step") {
    SmokeRunner runner;
    runner.add(std::make_unique<ScriptedStep>("first", 1));
    runner.add(std::make_unique<ScriptedStep>("breaks", 5, 2));
    runner.add(std::make_unique<ScriptedStep>("never", 1));
    SmokeStepContext context;
    const SmokeRunResult result = run(runner, context);
    CHECK_MESSAGE(
        (result.status == SmokeRunStatus::Failed && result.step == "breaks" && result.error == "scripted failure"),
        "a failed step fails the run with its name and error");
    CHECK_MESSAGE(context.log.back() == "breaks:2", "no step runs after a failure");
    const SmokeRunResult again = runner.update(context);
    CHECK_MESSAGE(
        (again.status == SmokeRunStatus::Failed && again.step == "breaks" && context.log.back() == "breaks:2"),
        "a failed run keeps reporting the same failure without running steps");
}

TEST_CASE("SmokeRunner: budget overrun fails") {
    SmokeRunner runner;
    runner.add(std::make_unique<ScriptedStep>("slow", 100, UINT32_MAX, 10));
    runner.add(std::make_unique<ScriptedStep>("after", 1));
    SmokeStepContext context;
    const SmokeRunResult result = run(runner, context);
    CHECK_MESSAGE((result.status == SmokeRunStatus::Failed && result.step == "slow" &&
                   result.error.find("10 frames") != std::string::npos),
                  "a step still running after its frame budget fails the run");
    CHECK_MESSAGE((context.log.size() == 10 && context.log.back() == "slow:9"),
                  "a step gets exactly its budget of updates");

    SmokeRunner exact;
    exact.add(std::make_unique<ScriptedStep>("exact", 10, UINT32_MAX, 10));
    SmokeStepContext exactContext;
    CHECK_MESSAGE(run(exact, exactContext).status == SmokeRunStatus::Passed,
                  "a step finishing on the last frame of its budget passes");
}
} // namespace

TEST_SUITE_END();
