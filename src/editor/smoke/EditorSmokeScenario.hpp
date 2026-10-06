#pragma once

// The editor smoke scenario: every step in order, run against the opened scene.

#include "editor/smoke/SmokeRunner.hpp"

#include <filesystem>
#include <memory>
#include <string>

struct ApplicationContext;
struct SceneDocumentOptions;

struct EditorSmokeScenario {
    // Null when the scenario could not be prepared.
    std::unique_ptr<SmokeRunner> runner;
    std::string error;
};

// The editor smoke (ZENITH_EDITOR_SMOKE): every step in order, run against
// the scene the editor opened. scratchDirectory holds the files the steps
// write (the editor's settings directory, which tests give their own).
EditorSmokeScenario buildEditorSmokeScenario(ApplicationContext& app, const SceneDocumentOptions& options,
                                             const std::filesystem::path& scratchDirectory);
