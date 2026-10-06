#pragma once

// The fast Ninja build of a project in <project>/.zenith/build with the engine's compiler environment.

#include "editor/project/BuildRunner.hpp"
#include "editor/project/DeveloperEnvironment.hpp"

#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <string_view>

// Where a generated project's Ninja build tree lives: under the project's
// editor-state folder, beside its own preset build tree (build/).
inline constexpr std::string_view fastBuildDirectoryName = ".zenith/build";

// The editor's fast build of a project's game module, run with environment's
// compiler: a Ninja build tree in <project>/.zenith/build, configured only
// while its build.ninja is missing (Ninja reconfigures itself when a
// CMakeLists.txt or the engine's config changes), and built with
// `cmake --build`. The project's Zenith preset says which engine and which
// configuration. Nullopt when projectFile has no such preset.
std::optional<BuildRequest> fastProjectBuildRequest(const std::filesystem::path& projectFile,
                                                    const DeveloperEnvironment& environment);

// The fast build of the open project, once its developer environment is
// captured: captureDeveloperEnvironment runs in the background from
// construction, because running vcvars64.bat takes a second or two. The
// project's preset build (build/, for its IDE) is left as it is; both copy
// the module to modules/, so hot reload works whichever built it.
class FastProjectBuild {
  public:
    // cacheFile keeps the captured environment between editor runs.
    FastProjectBuild(std::filesystem::path projectFile, std::filesystem::path cacheFile);
    // Waits for the capture.
    ~FastProjectBuild();
    FastProjectBuild(const FastProjectBuild&) = delete;
    FastProjectBuild& operator=(const FastProjectBuild&) = delete;

    // True while the environment is still being captured.
    bool pending();
    // The fast request; nullopt while pending or when the fast build cannot
    // run (see unavailableReason).
    std::optional<BuildRequest> request();
    // Why request() is nullopt once the capture finished; empty otherwise.
    const std::string& unavailableReason();

  private:
    void settle();

    std::filesystem::path m_projectFile;
    std::future<DeveloperEnvironmentResult> m_capture;
    std::optional<DeveloperEnvironmentResult> m_result;
    std::string m_unavailable;
};
