#include "GameLaunch.hpp"

#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#endif

namespace {
#if defined(_WIN32)
std::string utf8FromWide(const wchar_t* wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}
#endif

constexpr std::string_view projectExtension = ".zenith-project";
constexpr std::string_view packageManifest = "content-manifest.json";

GameLaunchResult readValue(std::span<const std::string_view> arguments, size_t& index, std::string_view option,
                           std::filesystem::path& value) {
    if (!value.empty()) {
        return {.error = std::string(option) + " was given more than once"};
    }
    if (index + 1 >= arguments.size() || arguments[index + 1].empty()) {
        return {.error = std::string(option) + " needs a file"};
    }
    ++index;
    value = pathFromUtf8(arguments[index]);
    return {.success = true};
}
} // namespace

GameLaunchResult parseGameLaunchArguments(std::span<const std::string_view> arguments, GameLaunchOptions& options) {
    GameLaunchOptions parsed;
    bool verifySeen = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        GameLaunchResult result{.success = true};
        if (argument == "--verify-package") {
            if (verifySeen) {
                return {.error = "--verify-package was given more than once"};
            }
            verifySeen = true;
            parsed.verifyPackage = true;
        } else if (argument == "--project") {
            result = readValue(arguments, index, argument, parsed.projectFile);
        } else if (argument == "--scene") {
            result = readValue(arguments, index, argument, parsed.sceneFile);
        } else {
            return {.error = "Unknown argument '" + std::string(argument) + "'"};
        }
        if (!result) {
            return result;
        }
    }
    if (!parsed.sceneFile.empty()) {
        std::error_code error;
        parsed.sceneFile = std::filesystem::absolute(parsed.sceneFile, error).lexically_normal();
        if (error) {
            return {.error = "--scene path cannot be made absolute: " + error.message()};
        }
    }
    options = std::move(parsed);
    return {.success = true};
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8FromPath(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::vector<std::string> utf8CommandLine(int argc, char** argv) {
#if defined(_WIN32)
    // argv is in the ANSI code page, which cannot spell every path; the wide
    // command line can.
    static_cast<void>(argc);
    static_cast<void>(argv);
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> arguments;
    if (wide != nullptr) {
        for (int index = 0; index < count; ++index) {
            arguments.push_back(utf8FromWide(wide[index]));
        }
        LocalFree(wide);
    }
    return arguments;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

GameLaunchResult findPackagedProject(const std::filesystem::path& directory, std::filesystem::path& projectFile) {
    projectFile.clear();
    std::error_code error;
    if (!std::filesystem::is_regular_file(directory / packageManifest, error)) {
        return {.success = true};
    }
    std::vector<std::filesystem::path> projects;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == projectExtension) {
            projects.push_back(entry.path());
        }
    }
    if (error) {
        return {.error = "Could not list the package at " + utf8FromPath(directory) + ": " + error.message()};
    }
    if (projects.size() > 1) {
        return {.error = "The package at " + utf8FromPath(directory) + " holds " + std::to_string(projects.size()) +
                         " project files; choose one with --project"};
    }
    if (!projects.empty()) {
        projectFile = projects.front();
    }
    return {.success = true};
}

GameLaunchResult loadLaunchProject(const GameLaunchOptions& options, const std::filesystem::path& resourceRoot,
                                   std::optional<ProjectConfig>& project) {
    std::filesystem::path projectFile = options.projectFile;
    if (projectFile.empty()) {
        if (GameLaunchResult result = findPackagedProject(resourceRoot, projectFile); !result) {
            return result;
        }
    }
    std::error_code error;
    if (!options.sceneFile.empty() && !std::filesystem::is_regular_file(options.sceneFile, error)) {
        return {.error = "--scene names a scene file that does not exist: " + utf8FromPath(options.sceneFile)};
    }
    if (projectFile.empty()) {
        if (options.sceneFile.empty()) {
            project.reset();
            return {.success = true};
        }
        // A scene without a project plays with the runtime's own assets.
        project = ProjectConfig{.displayName = utf8FromPath(options.sceneFile.stem()),
                                .assetRoot = resourceRoot / "assets",
                                .sceneRoot = options.sceneFile.parent_path(),
                                .startupScene = options.sceneFile};
        return {.success = true};
    }

    ProjectConfig loaded;
    if (const ProjectConfigResult result = loadProjectConfig(projectFile, loaded); !result) {
        return {.error = result.error};
    }
    if (!options.sceneFile.empty()) {
        loaded.startupScene = options.sceneFile;
        // A scene outside the scene root (an editor's copy) brings its own
        // folder as the scene root, for the scene references it holds.
        const std::filesystem::path relative = options.sceneFile.lexically_relative(loaded.sceneRoot);
        if (relative.empty() || *relative.begin() == "..") {
            loaded.sceneRoot = options.sceneFile.parent_path();
        }
    }
    if (loaded.startupScene.empty()) {
        return {.error = "Project '" + loaded.displayName + "' does not configure a startup scene; pass --scene"};
    }
    project = std::move(loaded);
    return {.success = true};
}
