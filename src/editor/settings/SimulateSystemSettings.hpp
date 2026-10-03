#pragma once

#include "engine/module/SystemScheduler.hpp"

#include <filesystem>
#include <string>

// The systems the user chose for Simulate, kept in simulate-systems.json in
// the editor's settings directory:
//
//   {"version": 1, "systems": {"move_platforms": true, "follow_camera": false}}
//
// Only explicit choices are stored; a system not listed uses
// SimulateSystemMask's default (FixedUpdate systems on, Update systems off).

struct SimulateSystemSettingsResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// A missing file leaves an empty mask and succeeds. An unreadable or invalid
// file also leaves an empty mask, and reports why.
SimulateSystemSettingsResult loadSimulateSystemSettings(const std::filesystem::path& path, SimulateSystemMask& mask);
SimulateSystemSettingsResult saveSimulateSystemSettings(const std::filesystem::path& path,
                                                        const SimulateSystemMask& mask);
