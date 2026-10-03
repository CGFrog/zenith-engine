#include "PackageRunner.hpp"

#include "editor/project/ProjectFileOperations.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace {
// The cache entries packaging configures; CMakeLists.txt reads them.
constexpr std::string_view projectVariable = "ZENITH_PACKAGE_PROJECT";
constexpr std::string_view moduleCopyVariable = "ZENITH_COPY_PROJECT_MODULE";
// Package errors published to the editor diagnostics at most.
constexpr size_t maximumPublishedErrors = 50;

// Paths as UTF-8 with forward slashes, which CMake reads on every platform.
std::string cmakePath(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

// The value of a NAME:TYPE=value line in CMakeCache.txt.
std::optional<std::string> cacheValue(std::string_view cacheText, std::string_view name) {
    std::istringstream lines{std::string(cacheText)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.starts_with(name) || line.size() <= name.size() || line[name.size()] != ':') {
            continue;
        }
        const size_t separator = line.find('=', name.size());
        if (separator != std::string::npos) {
            return line.substr(separator + 1);
        }
    }
    return std::nullopt;
}

bool cmakeFalse(std::string_view value) {
    std::string lower(value);
    std::ranges::transform(lower, lower.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
    });
    return lower == "off" || lower == "0" || lower == "false" || lower == "no" || lower == "n" ||
           lower.ends_with("-notfound") || lower == "notfound" || lower == "ignore";
}

bool samePath(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code error;
    if (std::filesystem::exists(first, error) && std::filesystem::exists(second, error)) {
        return std::filesystem::equivalent(first, second, error);
    }
    return first.lexically_normal() == second.lexically_normal();
}

// Runs each step through a BuildRunner and hands its new output lines over.
class ProcessPackageStepRunner final : public PackageStepRunner {
  public:
    ChildProcessResult start(const PackageStep& step, const std::filesystem::path& workingDirectory) override {
        return m_runner.start(
            {.command = step.label, .arguments = step.arguments, .workingDirectory = workingDirectory});
    }

    PackageStepPoll poll() override {
        const BuildPollResult polled = m_runner.poll();
        PackageStepPoll result{.finished = polled.finished, .state = m_runner.state(), .exitCode = m_runner.exitCode()};
        const std::deque<BuildOutputLine>& output = m_runner.output();
        // The log keeps its most recent lines, so a burst larger than it
        // arrives truncated.
        const size_t taken = std::min(polled.newLines, output.size());
        result.lines.assign(output.end() - static_cast<std::ptrdiff_t>(taken), output.end());
        return result;
    }

    void cancel() override {
        m_runner.cancel();
    }

  private:
    BuildRunner m_runner;
};
} // namespace

PackagePlatform hostPackagePlatform() {
#if defined(_WIN32)
    return PackagePlatform::Windows;
#elif defined(__APPLE__)
    return PackagePlatform::MacOS;
#else
    return PackagePlatform::Linux;
#endif
}

std::string_view packagePlatformLabel(PackagePlatform platform) {
    switch (platform) {
    case PackagePlatform::Windows:
        return "Windows";
    case PackagePlatform::Linux:
        return "Linux";
    case PackagePlatform::MacOS:
        return "macOS";
    }
    return "Unknown";
}

std::string_view packageConfigurationName(PackageConfiguration configuration) {
    switch (configuration) {
    case PackageConfiguration::Release:
        return "Release";
    case PackageConfiguration::RelWithDebInfo:
        return "RelWithDebInfo";
    }
    return "Release";
}

PackagePlatformSupport packagePlatformSupport(PackagePlatform platform, PackagePlatform host) {
    if (platform == host) {
        return {.available = true};
    }
    return {.reason = "Packages are built on the platform they ship for; build on a " +
                      std::string(packagePlatformLabel(platform)) + " computer to package for " +
                      std::string(packagePlatformLabel(platform)) + "."};
}

PackagePlanResult planPackage(const PackageRequest& request, PackagePlatform host) {
    if (request.projectFile.empty()) {
        return {.error = "No project is open. Start the editor with a .zenith-project file to package it."};
    }
    if (request.projectFile.extension() != ".zenith-project") {
        return {.error = "'" + utf8PathText(request.projectFile) + "' is not a .zenith-project file."};
    }
    if (const PackagePlatformSupport support = packagePlatformSupport(request.platform, host); !support.available) {
        return {.error = support.reason};
    }
    if (request.sourceDirectory.empty() || request.buildDirectory.empty()) {
        return {.error = "Packaging needs both the engine source directory and a release build directory."};
    }
    const auto folder = [](const std::filesystem::path& path) {
        const std::filesystem::path normal = path.lexically_normal();
        return !normal.has_filename() && normal.has_relative_path() ? normal.parent_path() : normal;
    };
    if (folder(request.buildDirectory) == folder(request.sourceDirectory)) {
        return {.error = "The package build directory must not be the source directory; set package.directory "
                         "to a folder of its own, such as build-release."};
    }
    if (request.cmakeProgram.empty()) {
        return {.error = "No CMake program is configured for packaging."};
    }

    const std::string configuration(packageConfigurationName(request.configuration));
    const std::string build = cmakePath(request.buildDirectory);
    PackagePlan plan{.buildDirectory = request.buildDirectory,
                     .projectFile = request.projectFile,
                     .platform = request.platform,
                     .configuration = request.configuration};
    if (request.configure) {
        // Tests are not packaged, and the release module must never replace
        // the copy beside the source project the editor loads.
        plan.steps.push_back({.label = "Configure " + configuration,
                              .arguments = {request.cmakeProgram, "-S", cmakePath(request.sourceDirectory), "-B", build,
                                            "-DCMAKE_BUILD_TYPE=" + configuration, "-DBUILD_TESTING=OFF",
                                            "-D" + std::string(projectVariable) + "=" + cmakePath(request.projectFile),
                                            "-D" + std::string(moduleCopyVariable) + "=OFF"}});
    }
    plan.steps.push_back(
        {.label = "Build " + configuration,
         .arguments = {request.cmakeProgram, "--build", build, "--config", configuration, "--parallel"}});
    plan.steps.push_back({.label = "Cook textures",
                          .arguments = {request.cmakeProgram, "--build", build, "--config", configuration, "--target",
                                        "zenith_cooked_textures"}});
    plan.steps.push_back(
        {.label = "Package",
         .arguments = {request.cmakeProgram, "--build", build, "--config", configuration, "--target", "package"}});
    return {.plan = std::move(plan)};
}

bool packageNeedsConfigure(std::string_view cacheText, const std::filesystem::path& projectFile,
                           PackageConfiguration configuration) {
    const std::optional<std::string> project = cacheValue(cacheText, projectVariable);
    if (!project || !samePath(pathFromUtf8Text(*project), projectFile)) {
        return true;
    }
    const std::optional<std::string> moduleCopy = cacheValue(cacheText, moduleCopyVariable);
    if (!moduleCopy || !cmakeFalse(*moduleCopy)) {
        return true;
    }
    // A single-configuration generator builds the configuration it was
    // configured with; a multi-configuration one picks it per build.
    if (!cacheValue(cacheText, "CMAKE_CONFIGURATION_TYPES")) {
        const std::optional<std::string> buildType = cacheValue(cacheText, "CMAKE_BUILD_TYPE");
        return !buildType || *buildType != packageConfigurationName(configuration);
    }
    return false;
}

bool packageCacheNeedsConfigure(const std::filesystem::path& buildDirectory, const std::filesystem::path& projectFile,
                                PackageConfiguration configuration) {
    std::ifstream cache(buildDirectory / "CMakeCache.txt", std::ios::binary);
    if (!cache) {
        return true;
    }
    const std::string text{std::istreambuf_iterator<char>(cache), std::istreambuf_iterator<char>()};
    return packageNeedsConfigure(text, projectFile, configuration);
}

std::optional<std::filesystem::path> newestPackageArchive(const std::filesystem::path& directory) {
    constexpr std::array<std::string_view, 6> archiveSuffixes{".zip", ".tar.gz", ".tgz", ".deb", ".dmg", ".msi"};
    std::optional<std::filesystem::path> newest;
    std::filesystem::file_time_type newestTime{};
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::string name = utf8PathText(entry.path().filename());
        const bool archive =
            std::ranges::any_of(archiveSuffixes, [&](std::string_view suffix) { return name.ends_with(suffix); });
        const std::filesystem::file_time_type written = entry.last_write_time(error);
        if (archive && !error && (!newest || written > newestTime)) {
            newest = entry.path();
            newestTime = written;
        }
    }
    return newest;
}

std::unique_ptr<PackageStepRunner> makeProcessPackageStepRunner() {
    return std::make_unique<ProcessPackageStepRunner>();
}

PackageRunner::PackageRunner(std::unique_ptr<PackageStepRunner> steps)
    : m_steps(std::move(steps)) {}

ChildProcessResult PackageRunner::start(PackagePlan plan) {
    if (running()) {
        return {.error = "A package is already being built"};
    }
    if (m_steps == nullptr) {
        return {.error = "Packaging has no step runner"};
    }
    if (plan.steps.empty()) {
        return {.error = "The package plan has no steps"};
    }
    std::error_code error;
    std::filesystem::create_directories(plan.buildDirectory, error);
    if (error) {
        return {.error = "Could not create the package build directory '" + utf8PathText(plan.buildDirectory) +
                         "': " + error.message()};
    }
    m_plan = std::move(plan);
    m_output.clear();
    m_exitCode.reset();
    m_failure.clear();
    m_cancelRequested = false;
    m_state = PackageState::Running;
    if (!startStep(0)) {
        return {.error = m_failure};
    }
    return {.success = true};
}

PackagePollResult PackageRunner::poll() {
    if (m_state != PackageState::Running) {
        return {};
    }
    PackageStepPoll polled = m_steps->poll();
    PackagePollResult result{.newLines = polled.lines.size()};
    for (BuildOutputLine& line : polled.lines) {
        appendLine(std::move(line));
    }
    if (!polled.finished) {
        return result;
    }
    result.finished = true;
    if (polled.state == BuildState::Cancelled || (m_cancelRequested && polled.state == BuildState::Succeeded)) {
        m_state = PackageState::Cancelled;
        return result;
    }
    if (polled.state != BuildState::Succeeded) {
        m_exitCode = polled.exitCode;
        const PackageStep& step = m_plan.steps[m_currentStep];
        fail("Step " + std::to_string(m_currentStep + 1) + " of " + std::to_string(m_plan.steps.size()) + " (" +
             step.label + ") failed with exit code " + std::to_string(polled.exitCode.value_or(-1)));
        return result;
    }
    if (m_currentStep + 1 == m_plan.steps.size()) {
        m_state = PackageState::Succeeded;
        return result;
    }
    // The next step keeps the package running.
    result.finished = !startStep(m_currentStep + 1);
    return result;
}

void PackageRunner::cancel() {
    if (!running()) {
        return;
    }
    m_cancelRequested = true;
    m_steps->cancel();
}

PackageState PackageRunner::state() const {
    return m_state;
}

bool PackageRunner::running() const {
    return m_state == PackageState::Running;
}

const PackagePlan& PackageRunner::plan() const {
    return m_plan;
}

size_t PackageRunner::currentStep() const {
    return m_currentStep;
}

std::optional<int> PackageRunner::exitCode() const {
    return m_exitCode;
}

const std::string& PackageRunner::failure() const {
    return m_failure;
}

const std::deque<BuildOutputLine>& PackageRunner::output() const {
    return m_output;
}

bool PackageRunner::startStep(size_t index) {
    m_currentStep = index;
    const PackageStep& step = m_plan.steps[index];
    appendLine({.text = "== [" + std::to_string(index + 1) + "/" + std::to_string(m_plan.steps.size()) + "] " +
                        step.label + " =="});
    if (const ChildProcessResult started = m_steps->start(step, m_plan.buildDirectory); !started) {
        fail("Could not start step " + std::to_string(index + 1) + " (" + step.label + "): " + started.error);
        return false;
    }
    return true;
}

void PackageRunner::fail(std::string message) {
    m_state = PackageState::Failed;
    m_failure = std::move(message);
    appendLine({.text = m_failure, .severity = CompilerDiagnosticSeverity::Error});
}

void PackageRunner::appendLine(BuildOutputLine line) {
    m_output.push_back(std::move(line));
    if (m_output.size() > maximumOutputLines) {
        m_output.pop_front();
    }
}

std::vector<EditorDiagnostic> packageDiagnostics(const PackageRunner& runner,
                                                 const std::optional<std::filesystem::path>& archive) {
    std::vector<EditorDiagnostic> diagnostics;
    std::set<std::string> published;
    for (const BuildOutputLine& line : runner.output()) {
        // The failure summary below covers the runner's own error line.
        if (line.severity != CompilerDiagnosticSeverity::Error || line.text == runner.failure() ||
            diagnostics.size() == maximumPublishedErrors || !published.insert(line.text).second) {
            continue;
        }
        diagnostics.push_back(
            {.severity = EditorDiagnosticSeverity::Error, .message = line.text, .location = line.location});
    }
    const PackagePlan& plan = runner.plan();
    const std::string what = "the " + std::string(packagePlatformLabel(plan.platform)) + " " +
                             std::string(packageConfigurationName(plan.configuration)) + " package of '" +
                             utf8PathText(plan.projectFile.filename()) + "'";
    switch (runner.state()) {
    case PackageState::Succeeded:
        diagnostics.push_back(
            {.severity = EditorDiagnosticSeverity::Info,
             .message = "Built " + what +
                        (archive ? ": " + utf8PathText(*archive) : " in " + utf8PathText(plan.buildDirectory))});
        break;
    case PackageState::Failed:
        diagnostics.push_back({.severity = EditorDiagnosticSeverity::Error,
                               .message = "Could not build " + what + ". " + runner.failure(),
                               .action = "Review the package output in the Build panel."});
        break;
    case PackageState::Cancelled:
        diagnostics.push_back({.severity = EditorDiagnosticSeverity::Info, .message = "Cancelled building " + what});
        break;
    case PackageState::Idle:
    case PackageState::Running:
        break;
    }
    return diagnostics;
}
