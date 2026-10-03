#pragma once

#include <string_view>

class ModuleRegistrar;

// An example component: an entity with a Spinner turns about its local up
// axis while the game runs (Play mode in the editor).
struct SpinnerComponent {
    // Degrees per second; negative turns the other way.
    float degreesPerSecond{90.0f};
};

// The key scenes store the component under, and the system's name.
inline constexpr std::string_view SpinnerComponentKey = "spinner";
inline constexpr std::string_view SpinSystemName = "{{PROJECT_IDENTIFIER}}.spin";

// Registers the Spinner component type (its fields are reflected with
// Fields<SpinnerComponent>, so the editor's inspector edits them) and the
// system that turns spinners.
void registerSpinnerGameplay(ModuleRegistrar& registrar);
