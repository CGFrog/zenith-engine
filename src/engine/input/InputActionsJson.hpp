#pragma once

// input_actions.json: reading and writing a project's input actions.

#include "InputActions.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// A project's default input actions as a JSON file, readable and editable by
// hand:
//
//   {"version": 1, "actions": [
//     {"name": "Move", "type": "axis2d", "clamp": true, "bindings": [
//       {"source": "key:W", "scale": [0, 1]},
//       {"source": "gamepadAxis:LeftX", "scale": [1, 0], "deadZone": 0.2}]},
//     {"name": "Sprint", "type": "button", "bindings": [
//       {"source": "key:LeftShift", "modifiers": ["key:LeftControl"]}]}]}
//
// Types are "button", "axis1d", and "axis2d"; clamp defaults to true. A
// binding's scale defaults to [1, 0], its modifiers to none, and its dead zone
// to 0.15. Sources are named "<kind>:<name>" (inputSourceName).

struct InputActionsJsonResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// "key:W", "mouseButton:Left", "mouseAxis:DeltaX", "gamepadButton:South", or
// "gamepadAxis:LeftX".
std::string inputSourceName(const InputBindingSource& source);
std::optional<InputBindingSource> parseInputSource(std::string_view name);
// Every source a binding can name, in kind and enum order, for pickers.
std::vector<InputBindingSource> allInputSources();
std::string_view inputActionTypeName(InputActionValueType type);
std::optional<InputActionValueType> parseInputActionType(std::string_view name);

std::string serializeInputActionMap(const InputActionMap& actions);
// Replaces actions with the document's; on failure actions is unchanged and
// error says which action or binding is wrong.
InputActionsJsonResult deserializeInputActionMap(std::string_view text, InputActionMap& actions);
InputActionsJsonResult saveInputActionMap(const std::filesystem::path& path, const InputActionMap& actions);
InputActionsJsonResult loadInputActionMap(const std::filesystem::path& path, InputActionMap& actions);
