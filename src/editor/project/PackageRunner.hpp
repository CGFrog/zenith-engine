#pragma once

// Build and Package: configure, build, cook, and package steps run one at a time.

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/project/BuildRunner.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Build and Package: the release packaging of docs/RELEASING.md run from the
// editor for the open project. A package is planned as a list of commands
// (configure, build, cook textures, package) that the PackageRunner runs one
// after another in a release build tree of its own, never the editor's.

enum class PackagePlatform : uint8_t {
    Windows,
    Linux,
    MacOS,
};

enum class PackageConfiguration : uint8_t {
    Release,
    RelWithDebInfo,
};

// The platform the editor runs on; packages are only built for it.
PackagePlatform hostPackagePlatform();
std::string_view packagePlatformLabel(PackagePlatform platform);
// The CMake configuration name, for example "Release".
std::string_view packageConfigurationName(PackageConfiguration configuration);

struct PackagePlatformSupport {
    bool available{false};
    // Why a platform cannot be packaged here; empty when it can.
    std::string reason;
};

PackagePlatformSupport packagePlatformSupport(PackagePlatform platform, PackagePlatform host = hostPackagePlatform());

struct PackageRequest {
    // The engine checkout holding the root CMakeLists.txt.
    std::filesystem::path sourceDirectory;
    // The release build tree; never the tree the editor itself runs from.
    std::filesystem::path buildDirectory;
    // The .zenith-project the package ships (ZENITH_PACKAGE_PROJECT).
    std::filesystem::path projectFile;
    PackagePlatform platform{hostPackagePlatform()};
    PackageConfiguration configuration{PackageConfiguration::Release};
    // Whether to configure the build tree first (see packageNeedsConfigure).
    bool configure{true};
    // The CMake program, found on PATH by default.
    std::string cmakeProgram{"cmake"};
};

struct PackageStep {
    // Shown in the Build panel, for example "Build Release".
    std::string label;
    // The program and its arguments, run without a shell.
    std::vector<std::string> arguments;
};

struct PackagePlan {
    std::vector<PackageStep> steps;
    // Where every step runs and where CPack writes the archive.
    std::filesystem::path buildDirectory;
    std::filesystem::path projectFile;
    PackagePlatform platform{hostPackagePlatform()};
    PackageConfiguration configuration{PackageConfiguration::Release};
};

struct PackagePlanResult {
    PackagePlan plan;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// The commands that package request's project; pure, so it can be tested
// without running anything. Fails with a reason for a missing project, an
// unsupported platform, or a build tree that is the source tree.
PackagePlanResult planPackage(const PackageRequest& request, PackagePlatform host = hostPackagePlatform());

// Whether a build tree whose CMakeCache.txt holds cacheText (empty when there
// is none) must be configured before packaging projectFile: it has no cache,
// was configured for another project or configuration, or still copies the
// project's module into the source tree (which would make the editor reload
// a release module).
bool packageNeedsConfigure(std::string_view cacheText, const std::filesystem::path& projectFile,
                           PackageConfiguration configuration);
// The same for the CMakeCache.txt in buildDirectory.
bool packageCacheNeedsConfigure(const std::filesystem::path& buildDirectory, const std::filesystem::path& projectFile,
                                PackageConfiguration configuration);

// The newest package archive CPack wrote into directory, if any.
std::optional<std::filesystem::path> newestPackageArchive(const std::filesystem::path& directory);

struct PackageStepPoll {
    // Output read since the previous poll.
    std::vector<BuildOutputLine> lines;
    // True on the one poll that observes the step ending.
    bool finished{false};
    // Succeeded, Failed, or Cancelled once finished.
    BuildState state{BuildState::Running};
    std::optional<int> exitCode;
};

// Runs one package step at a time. PackageRunner drives it; tests replace
// the process-backed runner with a scripted one.
class PackageStepRunner {
  public:
    virtual ~PackageStepRunner() = default;

    virtual ChildProcessResult start(const PackageStep& step, const std::filesystem::path& workingDirectory) = 0;
    virtual PackageStepPoll poll() = 0;
    virtual void cancel() = 0;
};

// Runs steps as child processes through a BuildRunner, so compiler output is
// parsed into located diagnostics as it is for the Build panel.
std::unique_ptr<PackageStepRunner> makeProcessPackageStepRunner();

enum class PackageState : uint8_t {
    Idle,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

struct PackagePollResult {
    size_t newLines{0};
    // True on the one poll that observes the whole package ending.
    bool finished{false};
};

// Runs a PackagePlan's steps in order, stopping at the first that fails or
// when cancelled, and keeps their combined output. Called from the owning
// (UI) thread. Starting a plan never triggers a module reload: only the
// Build panel's own builds do.
class PackageRunner {
  public:
    static constexpr size_t maximumOutputLines = BuildRunner::maximumOutputLines;

    explicit PackageRunner(std::unique_ptr<PackageStepRunner> steps);

    ChildProcessResult start(PackagePlan plan);
    // Takes the running step's new output and starts the next step when it
    // succeeds.
    PackagePollResult poll();
    // Stops the running step; no further step starts.
    void cancel();

    PackageState state() const;
    bool running() const;
    const PackagePlan& plan() const;
    // The step running now, or the one the package stopped at.
    size_t currentStep() const;
    // The failing step's exit code.
    std::optional<int> exitCode() const;
    // What went wrong, for a failed package.
    const std::string& failure() const;
    const std::deque<BuildOutputLine>& output() const;

  private:
    bool startStep(size_t index);
    void fail(std::string message);
    void appendLine(BuildOutputLine line);

    std::unique_ptr<PackageStepRunner> m_steps;
    PackagePlan m_plan;
    PackageState m_state{PackageState::Idle};
    size_t m_currentStep{0};
    bool m_cancelRequested{false};
    std::optional<int> m_exitCode;
    std::string m_failure;
    std::deque<BuildOutputLine> m_output;
};

// The diagnostics (source "Package") describing a finished package: the
// errors its steps printed and a summary of the outcome.
std::vector<EditorDiagnostic> packageDiagnostics(const PackageRunner& runner,
                                                 const std::optional<std::filesystem::path>& archive);
