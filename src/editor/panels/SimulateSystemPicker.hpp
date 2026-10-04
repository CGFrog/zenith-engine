#pragma once

#include "engine/module/SystemScheduler.hpp"

#include <string>
#include <vector>

class GameModuleHost;
struct EditorContext;

// One game module system Simulate can run, as the picker lists it.
struct SimulateSystemChoice {
    std::string name;
    SystemPhase phase{SystemPhase::Update};
    // Whether the Simulate mask runs it (GameModuleHost::simulateMask).
    bool enabled{false};
};

// The module's systems that run in Simulate when enabled (the Play systems),
// in phase order (Update first), each in its scheduler order, with its mask
// state.
std::vector<SimulateSystemChoice> simulateSystemChoices(const GameModuleHost& modules);

// The checkbox label the picker gives choice: its name, with its phase for
// systems outside Update ("move (fixed)"), and the picker's id suffix.
std::string simulateSystemLabel(const SimulateSystemChoice& choice);

// The Simulate system picker: a checkbox per choice, each change running
// play.set_simulate_system. Drawn inline in the current window, menu, or
// popup; the System Profiler, the Play menu, and the playback toolbar share it.
void drawSimulateSystemPicker(EditorContext& context, const GameModuleHost& modules);

// A button opening drawSimulateSystemPicker in a popover below it.
void drawSimulateSystemPickerButton(EditorContext& context, const GameModuleHost& modules,
                                    const char* label = "Simulate Systems");
