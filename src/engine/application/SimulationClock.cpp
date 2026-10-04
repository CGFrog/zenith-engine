#include "SimulationClock.hpp"

#include <algorithm>
#include <cmath>

namespace {
// Negative and NaN steps hold the simulation still.
float frameStep(const SimulationClock& clock) {
    return clock.frameStepSeconds > 0.0f ? clock.frameStepSeconds : 0.0f;
}
} // namespace

float clampTimeScale(float scale) {
    if (std::isnan(scale)) {
        return 1.0f;
    }
    return std::clamp(scale, SimulationClock::minimumTimeScale, SimulationClock::maximumTimeScale);
}

float simulatedSeconds(const SimulationClock& clock, float frameSeconds) {
    return clock.paused ? frameStep(clock) : frameSeconds * clampTimeScale(clock.timeScale);
}

float fixedStepAccumulation(const SimulationClock& clock, float frameSeconds) {
    return simulatedSeconds(clock, frameSeconds);
}

float audioPlaybackRate(const SimulationClock& clock) {
    if (clock.paused) {
        return frameStep(clock) > 0.0f ? 1.0f : 0.0f;
    }
    return clampTimeScale(clock.timeScale);
}
