#include "FastProjectBuild.hpp"

#include "editor/project/ProjectFileOperations.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/project/TemplateText.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <utility>

namespace {
using Json = nlohmann::json;

struct PresetSettings {
    std::filesystem::path engineBuildDirectory;
    std::string configuration;
};

// The engine (ZENITH_ROOT) and configuration of the project's Zenith preset.
std::optional<PresetSettings> presetSettings(const std::filesystem::path& projectFile) {
    std::string contents;
    if (projectFile.empty() || !readTextFile(projectFile.parent_path() / "CMakePresets.json", contents)) {
        return std::nullopt;
    }
    const Json presets = Json::parse(contents, nullptr, false);
    if (!presets.is_object()) {
        return std::nullopt;
    }
    PresetSettings settings;
    for (const Json& preset : presets.value("configurePresets", Json::array())) {
        if (!preset.is_object() || preset.value("name", std::string{}) != projectBuildPresetName) {
            continue;
        }
        const Json root = preset.value("cacheVariables", Json::object()).value("ZENITH_ROOT", Json());
        const std::string path = root.is_object()   ? root.value("value", std::string{})
                                 : root.is_string() ? root.get<std::string>()
                                                    : std::string{};
        settings.engineBuildDirectory = pathFromUtf8Text(path);
        settings.configuration = preset.value("cacheVariables", Json::object()).value("CMAKE_BUILD_TYPE", "");
    }
    for (const Json& preset : presets.value("buildPresets", Json::array())) {
        if (preset.is_object() && preset.value("name", std::string{}) == projectBuildPresetName) {
            settings.configuration = preset.value("configuration", settings.configuration);
        }
    }
    if (settings.engineBuildDirectory.empty() || settings.configuration.empty()) {
        return std::nullopt;
    }
    return settings;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}
} // namespace

std::optional<BuildRequest> fastProjectBuildRequest(const std::filesystem::path& projectFile,
                                                    const DeveloperEnvironment& environment) {
    const std::optional<PresetSettings> settings = presetSettings(projectFile);
    if (!settings) {
        return std::nullopt;
    }
    const std::filesystem::path project = projectFile.parent_path();
    const std::filesystem::path buildTree = project / fastBuildDirectoryName;
    std::vector<std::string> configure{"cmake",
                                       "-S",
                                       utf8(project),
                                       "-B",
                                       utf8(buildTree),
                                       "-G",
                                       "Ninja",
                                       "-DCMAKE_BUILD_TYPE=" + settings->configuration,
                                       "-DCMAKE_MAKE_PROGRAM=" + utf8(environment.ninja),
                                       "-DZENITH_ROOT=" + utf8(settings->engineBuildDirectory)};
#ifdef _WIN32
    // The developer environment's compiler, not another one on PATH.
    configure.emplace_back("-DCMAKE_CXX_COMPILER=cl");
#endif
    return BuildRequest{.command = "cmake --build " + std::string(fastBuildDirectoryName) + " (Ninja)",
                        .arguments = {"cmake", "--build", utf8(buildTree)},
                        .workingDirectory = project,
                        .environment = environment.variables,
                        .prepareArguments = std::move(configure),
                        .prepareUnlessExists = buildTree / "build.ninja"};
}

FastProjectBuild::FastProjectBuild(std::filesystem::path projectFile, std::filesystem::path cacheFile)
    : m_projectFile(std::move(projectFile)) {
    const std::optional<PresetSettings> settings = presetSettings(m_projectFile);
    if (!settings) {
        m_result = DeveloperEnvironmentResult{.error = "the project has no Zenith preset"};
        return;
    }
    m_capture = std::async(std::launch::async, [engine = settings->engineBuildDirectory, cache = std::move(cacheFile)] {
        return captureDeveloperEnvironment(engine, cache);
    });
}

FastProjectBuild::~FastProjectBuild() {
    if (m_capture.valid()) {
        m_capture.wait();
    }
}

void FastProjectBuild::settle() {
    if (!m_result && m_capture.valid() && m_capture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_result = m_capture.get();
        if (!m_result->environment) {
            m_unavailable = m_result->error;
        }
    }
    if (m_result && !m_result->environment && m_unavailable.empty()) {
        m_unavailable = m_result->error;
    }
}

bool FastProjectBuild::pending() {
    settle();
    return !m_result;
}

std::optional<BuildRequest> FastProjectBuild::request() {
    settle();
    if (!m_result || !m_result->environment) {
        return std::nullopt;
    }
    return fastProjectBuildRequest(m_projectFile, *m_result->environment);
}

const std::string& FastProjectBuild::unavailableReason() {
    settle();
    return m_unavailable;
}
