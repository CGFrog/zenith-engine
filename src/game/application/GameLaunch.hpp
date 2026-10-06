#pragma once

// What the zenith executable was asked to do, from its command line.

#include "engine/project/ProjectConfig.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the zenith executable was asked to do, from its command line.
struct GameLaunchOptions {
    // Check the package without a GPU instead of playing (PackageVerification).
    bool verifyPackage{false};
    // Empty when --project was not given: the project packaged beside the
    // executable, if there is one (findPackagedProject).
    std::filesystem::path projectFile;
    // Absolute; empty when --scene was not given: the project's startup scene.
    std::filesystem::path sceneFile;
};

struct GameLaunchResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

inline constexpr std::string_view gameUsage =
    "Usage: zenith [--verify-package] [--project <project-file>] [--scene <scene-file>]";

// The program's arguments, program name first, as UTF-8. On Windows they are
// read from the wide command line (argv is in the ANSI code page, which cannot
// spell every path); elsewhere argv is used as given.
std::vector<std::string> utf8CommandLine(int argc, char** argv);

// A path from UTF-8 text and back, whatever the platform's narrow encoding
// (path::string() throws on Windows for characters outside the code page).
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string utf8FromPath(const std::filesystem::path& path);

// Parses the UTF-8 arguments after the program name. Options may come in any order,
// each at most once. --scene names a .zenith-scene file to play instead of the
// project's startup scene (an editor's standalone play passes an absolute
// path; a relative one is resolved against the working directory). Leaves
// options unchanged on failure.
GameLaunchResult parseGameLaunchArguments(std::span<const std::string_view> arguments, GameLaunchOptions& options);

// The one .zenith-project file directly in directory when directory is a
// release package (it holds content-manifest.json), which places its project
// at its root; empty in any other directory, such as a build tree. More than
// one project in a package is an error, since the executable cannot choose.
GameLaunchResult findPackagedProject(const std::filesystem::path& directory, std::filesystem::path& projectFile);

// Loads the project to play: options.projectFile, else the project packaged in
// resourceRoot, else none (the built-in demo scene). The loaded project's
// startupScene is the scene to play: options.sceneFile when given, which must
// exist but need not lie under the scene root (when it does not, its folder
// becomes the scene root). A --scene without any project plays with the
// runtime's own assets (resourceRoot/assets), its folder as the scene root,
// and default render settings.
GameLaunchResult loadLaunchProject(const GameLaunchOptions& options, const std::filesystem::path& resourceRoot,
                                   std::optional<ProjectConfig>& project);
