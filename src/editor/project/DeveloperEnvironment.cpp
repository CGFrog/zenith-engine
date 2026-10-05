#include "DeveloperEnvironment.hpp"

#include "editor/project/ProjectFileOperations.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/project/TemplateText.hpp"
#include "engine/platform/ChildProcess.hpp"
#include "engine/platform/VersionedJson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

namespace {
using Json = nlohmann::json;

constexpr VersionedJsonFormat cacheFormat{.description = "developer environment cache"};

bool sameName(std::string_view left, std::string_view right) {
#ifdef _WIN32
    return std::ranges::equal(left, right, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
#else
    return left == right;
#endif
}

std::string variable(const std::vector<std::pair<std::string, std::string>>& variables, std::string_view name) {
    for (const auto& [key, value] : variables) {
        if (sameName(key, name)) {
            return value;
        }
    }
    const char* inherited = std::getenv(std::string(name).c_str());
    return inherited != nullptr ? inherited : std::string{};
}

// The first directory of a PATH value that holds program.
std::optional<std::filesystem::path> findOnPath(std::string_view path, std::string_view program) {
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    size_t begin = 0;
    while (begin <= path.size()) {
        const size_t end = std::min(path.find(separator, begin), path.size());
        if (end > begin) {
            const std::filesystem::path candidate = pathFromUtf8Text(path.substr(begin, end - begin)) / program;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                return candidate;
            }
        }
        begin = end + 1;
    }
    return std::nullopt;
}

DeveloperEnvironmentResult withNinja(std::vector<std::pair<std::string, std::string>> variables) {
#ifdef _WIN32
    constexpr std::string_view ninjaProgram = "ninja.exe";
#else
    constexpr std::string_view ninjaProgram = "ninja";
#endif
    const std::optional<std::filesystem::path> ninja = findOnPath(variable(variables, "PATH"), ninjaProgram);
    if (!ninja) {
        return {.error = "Ninja is not on the build environment's PATH"};
    }
    return {.environment = DeveloperEnvironment{.variables = std::move(variables), .ninja = *ninja}};
}

#ifdef _WIN32
int64_t writeTimeKey(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    return error ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
}

std::optional<std::vector<std::pair<std::string, std::string>>>
readCache(const std::filesystem::path& cacheFile, const std::string& instance, int64_t vcvarsTime) {
    const VersionedJsonResult read = readVersionedJson(cacheFile, cacheFormat);
    if (!read || !read.found || read.document.value("instance", std::string{}) != instance ||
        read.document.value("vcvars_write_time", int64_t{0}) != vcvarsTime) {
        return std::nullopt;
    }
    const auto stored = read.document.find("variables");
    if (stored == read.document.end() || !stored->is_object()) {
        return std::nullopt;
    }
    std::vector<std::pair<std::string, std::string>> variables;
    for (auto item = stored->begin(); item != stored->end(); ++item) {
        if (item.value().is_string()) {
            variables.emplace_back(item.key(), item.value().get<std::string>());
        }
    }
    return variables;
}
#endif
} // namespace

std::vector<std::pair<std::string, std::string>> parseEnvironmentListing(std::string_view listing) {
    std::vector<std::pair<std::string, std::string>> variables;
    size_t begin = 0;
    while (begin < listing.size()) {
        size_t end = listing.find('\n', begin);
        if (end == std::string_view::npos) {
            end = listing.size();
        }
        std::string_view line = listing.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const size_t separator = line.find('=');
        if (separator != std::string_view::npos && separator > 0) {
            variables.emplace_back(std::string(line.substr(0, separator)), std::string(line.substr(separator + 1)));
        }
        begin = end + 1;
    }
    return variables;
}

std::optional<std::filesystem::path> engineVisualStudioInstance(const std::filesystem::path& engineBuildDirectory) {
    std::string contents;
    if (!readTextFile(engineBuildDirectory / engineDescriptionFileName, contents)) {
        return std::nullopt;
    }
    const Json description = Json::parse(contents, nullptr, false);
    if (!description.is_object() ||
        description.value("generator", std::string{}).find("Visual Studio") == std::string::npos) {
        return std::nullopt;
    }
    const std::string instance = description.value("generatorInstance", std::string{});
    if (instance.empty()) {
        return std::nullopt;
    }
    return pathFromUtf8Text(instance);
}

DeveloperEnvironmentResult captureDeveloperEnvironment(const std::filesystem::path& engineBuildDirectory,
                                                       const std::filesystem::path& cacheFile) {
#ifdef _WIN32
    const std::optional<std::filesystem::path> instance = engineVisualStudioInstance(engineBuildDirectory);
    if (!instance) {
        return {.error = "The engine was not built with a Visual Studio generator"};
    }
    const std::filesystem::path vcvars = *instance / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    std::error_code error;
    if (!std::filesystem::is_regular_file(vcvars, error)) {
        return {.error = "The engine's Visual Studio has no vcvars64.bat at " + vcvars.string()};
    }
    const std::string instanceKey = instance->generic_string();
    const int64_t vcvarsTime = writeTimeKey(vcvars);
    if (std::optional<std::vector<std::pair<std::string, std::string>>> cached =
            readCache(cacheFile, instanceKey, vcvarsTime)) {
        return withNinja(std::move(*cached));
    }

    ChildProcess process;
    const ChildProcessResult started =
        process.start({.command = ChildProcessCommand::shell("call \"" + vcvars.string() + "\" >nul && set")});
    if (!started) {
        return {.error = "Could not run vcvars64.bat: " + started.error};
    }
    process.wait();
    if (process.exitCode() != 0) {
        return {.error = "vcvars64.bat failed with exit code " + std::to_string(process.exitCode().value_or(-1))};
    }
    std::string listing;
    for (const std::string& line : process.takeLines()) {
        listing += line;
        listing += '\n';
    }
    std::vector<std::pair<std::string, std::string>> variables = parseEnvironmentListing(listing);
    if (variable(variables, "VCToolsInstallDir").empty()) {
        return {.error = "vcvars64.bat did not set up the compiler (no VCToolsInstallDir)"};
    }
    Json stored = Json::object();
    for (const auto& [name, value] : variables) {
        stored[name] = value;
    }
    // A cache that cannot be written only costs the next capture's time.
    writeVersionedJson(cacheFile, cacheFormat,
                       Json{{"instance", instanceKey}, {"vcvars_write_time", vcvarsTime}, {"variables", stored}});
    return withNinja(std::move(variables));
#else
    (void)engineBuildDirectory;
    (void)cacheFile;
    return withNinja({});
#endif
}
