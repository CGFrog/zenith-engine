#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// What a smoke step hands its update. The runner only passes it through, so
// it is declared here and defined by the code that runs the steps
// (EditorSmokeHooks.hpp for the editor's scenario).
struct SmokeStepContext;

enum class SmokeStepStatus : uint8_t {
    // The step needs more frames.
    Running,
    // The step finished; the next one starts on the next frame.
    Done,
    // The step found a problem, described by SmokeStepResult::error.
    Failed,
};

struct SmokeStepResult {
    SmokeStepStatus status{SmokeStepStatus::Running};
    std::string error;

    static SmokeStepResult running() {
        return {};
    }

    static SmokeStepResult done() {
        return {.status = SmokeStepStatus::Done};
    }

    static SmokeStepResult failed(std::string error) {
        return {.status = SmokeStepStatus::Failed, .error = std::move(error)};
    }
};

// One step of a smoke scenario. Steps run one after another: a step's update
// is called once per frame, from the frame it becomes active until it returns
// Done or Failed, and never while another step runs.
class SmokeStep {
  public:
    virtual ~SmokeStep() = default;

    // Names the step in failure reports; unique within a scenario.
    virtual std::string_view name() const = 0;
    // stepFrame counts the frames since the step became active, from 0.
    virtual SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) = 0;

    // The frames the step may take: a step still Running after this many
    // updates fails, so a stuck step ends the run instead of hanging it.
    virtual uint32_t frameBudget() const {
        return defaultFrameBudget;
    }

    static constexpr uint32_t defaultFrameBudget = 60;
};

enum class SmokeRunStatus : uint8_t {
    Running,
    // Every step returned Done.
    Passed,
    // A step failed or ran over its budget; the run stops there.
    Failed,
};

struct SmokeRunResult {
    SmokeRunStatus status{SmokeRunStatus::Running};
    // The failed step's name and what went wrong; empty unless Failed.
    std::string step;
    std::string error;
};

// Runs smoke steps strictly in the order they were added, one frame at a time.
class SmokeRunner {
  public:
    void add(std::unique_ptr<SmokeStep> step);

    // Advances the active step by one frame. Once the run has passed or
    // failed, returns that result again without running anything.
    SmokeRunResult update(SmokeStepContext& context);

    size_t stepCount() const {
        return m_steps.size();
    }

  private:
    std::vector<std::unique_ptr<SmokeStep>> m_steps;
    size_t m_active{0};
    uint32_t m_stepFrame{0};
    SmokeRunResult m_result;
};
