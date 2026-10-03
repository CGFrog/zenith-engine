#include "SimulationClock.hpp"

#include <algorithm>
#include <cmath>

float clampTimeScale(float scale) {
    if (std::isnan(scale)) {
        return 1.0f;
    }
    return std::clamp(scale, SimulationClock::minimumTimeScale, SimulationClock::maximumTimeScale);
}

float simulatedSeconds(const SimulationClock& clock, float frameSeconds) {
    return clock.paused ? 0.0f : frameSeconds * clampTimeScale(clock.timeScale);
}

float fixedStepAccumulation(const SimulationClock& clock, float frameSeconds) {
    return clock.paused ? frameSeconds : frameSeconds * clampTimeScale(clock.timeScale);
}

float audioPlaybackRate(const SimulationClock& clock) {
    return clock.paused ? 0.0f : clampTimeScale(clock.timeScale);
}
