#pragma once

// Reusable pause and settings menus (drawPauseMenu, drawSettingsMenu).

#include "GameUi.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// What the player chose on a menu this frame.
enum class MenuAction {
    None,
    Resume,
    OpenSettings,
    Quit,
    Back,
    // A setting changed (its setter already ran); the game may persist it.
    Changed,
};

struct PauseMenuOptions {
    std::string title{"Paused"};
    std::string resumeLabel{"Resume"};
    std::string settingsLabel{"Settings"};
    std::string quitLabel{"Quit"};
    bool showSettings{true};
    bool showQuit{true};
};

// A pause screen centred on the canvas: the title, then Resume, Settings and
// Quit buttons.
MenuAction drawPauseMenu(GameUiContext& ui, const PauseMenuOptions& options = {});

// The controls a settings screen offers. Each reads the current value with
// get and applies a change with set; the game decides what a setting means.
struct SettingsToggle {
    std::function<bool()> get;
    std::function<void(bool)> set;
};

// Cycles through named options when activated.
struct SettingsChoice {
    std::vector<std::string> options;
    std::function<size_t()> get;
    std::function<void(size_t)> set;
};

struct SettingsSlider {
    float minimum{0.0f};
    float maximum{1.0f};
    float step{0.05f};
    std::function<float()> get;
    std::function<void(float)> set;
    // The value as shown beside the label; empty shows it with two decimals.
    std::function<std::string(float)> format;
};

struct SettingsEntry {
    // Unique among the screen's entries; the widget id is "settings.<id>".
    std::string id;
    std::string label;
    std::variant<SettingsToggle, SettingsChoice, SettingsSlider> control;
};

struct SettingsMenuOptions {
    std::string title{"Settings"};
    std::string backLabel{"Back"};
};

// A settings screen centred on the canvas: one row per entry, then Back.
// Returns Changed when an entry changed this frame, Back when Back was chosen.
MenuAction drawSettingsMenu(GameUiContext& ui, std::span<const SettingsEntry> entries,
                            const SettingsMenuOptions& options = {});
