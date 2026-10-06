#pragma once

// The environment a command-line build with the engine's compiler needs.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The environment a command-line build with the engine's compiler needs.
// On Windows that is what vcvars64.bat of the engine's Visual Studio
// sets (its generator instance, recorded in <engine build>/zenith-engine.json),
// captured once, so the editor can run Ninja builds without a developer
// prompt; elsewhere the editor's own environment already has the compiler.
struct DeveloperEnvironment {
    // Variables a build process gets in place of the editor's
    // (ChildProcessOptions::environment); empty elsewhere than Windows.
    std::vector<std::pair<std::string, std::string>> variables;
    // The Ninja executable found on the environment's PATH.
    std::filesystem::path ninja;
};

struct DeveloperEnvironmentResult {
    std::optional<DeveloperEnvironment> environment;
    // Why there is no environment, for the Build panel.
    std::string error;
};

// "NAME=value" lines (the `set` command's listing) as variables; lines
// without a name are skipped.
std::vector<std::pair<std::string, std::string>> parseEnvironmentListing(std::string_view listing);

// The Visual Studio instance the engine build at engineBuildDirectory was
// configured with; nullopt when its zenith-engine.json names none (another
// generator) or cannot be read.
std::optional<std::filesystem::path> engineVisualStudioInstance(const std::filesystem::path& engineBuildDirectory);

// Captures the environment for the engine build at engineBuildDirectory.
// Running vcvars64.bat takes a second or two, so the result is kept in
// cacheFile, keyed by the instance and vcvars64.bat's write time, and read
// from there while both still match. Blocks; run it off the UI thread.
DeveloperEnvironmentResult captureDeveloperEnvironment(const std::filesystem::path& engineBuildDirectory,
                                                       const std::filesystem::path& cacheFile);
