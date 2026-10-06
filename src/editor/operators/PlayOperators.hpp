#pragma once

// Headless operators that tune how Play and Simulate run.

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
//   play.apply_to_edit       copy runtime state back to the edit scene:
//                            {"entity": id or "" for the selection,
//                             "component": key or "" for the whole entity}
//
// The first three change editor state, never the scene, so they record no
// undo step. play.apply_to_edit runs only in Play or Simulate: it leaves the
// runtime scene alone and records one step on the edit history that patches
// the parked edit scene (SceneEditing::applyToEditScene), so the change is
// there after Stop and undoes and redoes there like any other edit.

struct ApplyToEditSceneProperties {
    // Stable id of the entity, in decimal; empty applies every selected entity.
    std::string entity;
    // Component type key; empty applies the whole entity (see
    // RuntimeApplyRequest).
    std::string component;
};

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

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerPlayOperatorFamily(OperatorRegistry& registry,
                                                      const EditorOperatorServices& services);
