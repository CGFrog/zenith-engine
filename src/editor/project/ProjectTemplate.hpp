#pragma once

#include "editor/project/BuildRunner.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class AssetManager;

// A Zenith build directory a generated project compiles its game module
// against holds ZenithEngineConfig.cmake (the imported Zenith::Engine target)
// and zenith-engine.json (the CMake generator and compiler it was configured
// with), both written when the engine is configured.
inline constexpr std::string_view engineConfigFileName = "ZenithEngineConfig.cmake";
inline constexpr std::string_view engineDescriptionFileName = "zenith-engine.json";
// The template under templates/ that New from Template uses.
inline constexpr std::string_view cppGameTemplateName = "cpp_game";
// The configure and build preset a generated project's CMakePresets.json
// defines, which the editor's Build command runs.
inline constexpr std::string_view projectBuildPresetName = "zenith";

// The first candidate holding ZenithEngineConfig.cmake, or empty when none
// does.
std::filesystem::path findEngineBuildRoot(std::span<const std::filesystem::path> candidates);
// The first templateRoots/templateName directory that exists, or empty.
std::filesystem::path findProjectTemplate(std::span<const std::filesystem::path> templateRoots,
                                          std::string_view templateName);

// The identifier a display name gives the project's folder, project file,
// CMake target, and module: lower-case ASCII letters, digits, and
// underscores, starting with a letter ("My Game 2" gives "my_game_2"), with
// "_game" appended to a name CMake reserves ("All" gives "all_game"). Empty
// when the name has no letter or digit.
std::string projectIdentifier(std::string_view displayName);

struct ProjectTemplateRequest {
    // ASCII letters, digits, spaces, '-' and '_', at most 64 characters.
    std::string displayName;
    // The project is created in parentDirectory/projectIdentifier(displayName),
    // which must not exist yet or be empty.
    std::filesystem::path parentDirectory;
    // The template directory (templates/cpp_game).
    std::filesystem::path templateDirectory;
    // The Zenith build the project's ZENITH_ROOT defaults to.
    std::filesystem::path engineRoot;
    // The engine's assets: the starter scene's model (primitives/cube.gltf)
    // is copied from here into the project's assets.
    std::filesystem::path engineAssetRoot;
    // The configuration the module builds in. It must be the editor's: a
    // module built with another runtime is refused by its build hash.
    std::string buildConfiguration{"Debug"};
};

struct ProjectTemplateResult {
    bool success{false};
    std::string error;
    // The generated .zenith-project file.
    std::filesystem::path projectFile;

    explicit operator bool() const {
        return success;
    }
};

// Generates a C++ game project from the template:
//
//   <identifier>.zenith-project  ProjectConfig v4 (display name, assets/,
//                                scenes/, scenes/main.zenith-scene, and
//                                modules/<identifier>.zmodule)
//   CMakeLists.txt, src/         the game module (one example component and
//                                system), built against ZENITH_ROOT
//   CMakePresets.json            the "zenith" configure and build presets
//   assets/, scenes/             the starter model and scene (ground, cube,
//                                sun, environment, camera)
//   README.md, .gitignore
//
// Every template file is copied with {{PROJECT_NAME}}, {{PROJECT_IDENTIFIER}},
// {{ZENITH_ROOT}}, and {{BUILD_CONFIG}} substituted in its contents and path;
// any other {{UPPER_CASE}} token is an error. On failure a directory the
// generator created is removed again. assets loads the starter model while
// the scene is written.
ProjectTemplateResult generateProjectFromTemplate(const ProjectTemplateRequest& request, AssetManager& assets);

// The build a generated project runs from its own directory: configure and
// build its "zenith" CMake preset. Empty when no CMakePresets.json beside the
// project file defines that preset.
std::optional<BuildRequest> projectBuildRequest(const std::filesystem::path& projectFile);
