#pragma once

#include "editor/operators/OperatorRegistry.hpp"

#include <functional>
#include <string>

class EditorPlayMode;
class GameModuleHost;

// Headless operators that tune how Play and Simulate run. Entering and
// leaving Play and Simulate need the running application (physics, audio),
// so EditorSession registers those (play.start, play.simulate, play.stop...).
//
//   play.set_time_scale      scale simulated time: {"scale": 0.0..10.0}
//   play.set_simulate_system choose whether Simulate runs a system:
//                            {"system": name, "enabled": bool} (internal)
//   profiler.reset_max       restart the System Profiler's maximum columns
//
// They change editor state, never the scene, so they record no undo step.

struct TimeScaleProperties {
    // Multiplier on simulated time; clamped to [0, 10].
    float scale{1.0f};
};

struct SimulateSystemProperties {
    std::string system;
    bool enabled{true};
};

struct PlayOperatorServices {
    EditorPlayMode& playMode;
    GameModuleHost& modules;
    // Called after play.set_simulate_system changes the Simulate mask, for
    // example to save it; may be empty.
    std::function<void()> simulateMaskChanged;
};

// The services must outlive the registered operators.
OperatorRegistrationResult registerPlayOperators(OperatorRegistry& registry, PlayOperatorServices services);
