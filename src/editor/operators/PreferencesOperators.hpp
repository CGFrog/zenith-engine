#pragma once

#include "editor/operators/OperatorRegistry.hpp"

class PreferencesWindow;

// Registers wm.preferences, which opens the Preferences window (Edit >
// Preferences). It needs no window to run, so it works headless; preferences
// must outlive the registry.
OperatorRegistrationResult registerPreferencesOperators(OperatorRegistry& registry, PreferencesWindow& preferences);
