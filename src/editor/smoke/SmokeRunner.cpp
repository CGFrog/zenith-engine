#include "SmokeRunner.hpp"

#include <utility>

void SmokeRunner::add(std::unique_ptr<SmokeStep> step) {
    m_steps.push_back(std::move(step));
}

SmokeRunResult SmokeRunner::update(SmokeStepContext& context) {
    if (m_result.status != SmokeRunStatus::Running) {
        return m_result;
    }
    if (m_active == m_steps.size()) {
        m_result.status = SmokeRunStatus::Passed;
        return m_result;
    }
    SmokeStep& step = *m_steps[m_active];
    const uint32_t stepFrame = m_stepFrame++;
    SmokeStepResult result = step.update(context, stepFrame);
    if (result.status == SmokeStepStatus::Running && m_stepFrame >= step.frameBudget()) {
        result = SmokeStepResult::failed("did not finish within " + std::to_string(step.frameBudget()) + " frames");
    }
    switch (result.status) {
    case SmokeStepStatus::Running:
        break;
    case SmokeStepStatus::Done:
        ++m_active;
        m_stepFrame = 0;
        if (m_active == m_steps.size()) {
            m_result.status = SmokeRunStatus::Passed;
        }
        break;
    case SmokeStepStatus::Failed:
        m_result = {.status = SmokeRunStatus::Failed, .step = std::string(step.name()), .error = result.error};
        break;
    }
    return m_result;
}
