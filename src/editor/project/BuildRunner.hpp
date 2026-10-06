#pragma once

// Runs one build command at a time without blocking its caller, keeping the output log and the diagnostics parsed from
// it.

#include "editor/project/CompilerOutputParser.hpp"
#include "engine/platform/ChildProcess.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct BuildRequest {
    // Run by the platform shell, so it may chain commands. With arguments it
    // is only the text shown for the build.
    std::string command;
    // When not empty: the program and its arguments, run directly with no
    // shell, so paths with spaces or shell characters stay single arguments.
    std::vector<std::string> arguments;
    // Empty runs in the editor's working directory. Relative diagnostic paths
    // are resolved against this directory.
    std::filesystem::path workingDirectory;
    // Variables the build's processes get in place of the editor's
    // (ChildProcessOptions::environment), for example a compiler's developer
    // environment.
    std::vector<std::pair<std::string, std::string>> environment;
    // When not empty, a program and its arguments run first while
    // prepareUnlessExists is missing, for example configuring a build tree
    // once; the build runs only when it succeeds. Its output joins the build's.
    std::vector<std::string> prepareArguments;
    std::filesystem::path prepareUnlessExists;
};

enum class BuildState : uint8_t {
    Idle,
    Running,
    Succeeded,
    Failed,
    Cancelled
};

struct BuildOutputLine {
    std::string text;
    // Set when the line is a compiler diagnostic, for highlighting.
    std::optional<CompilerDiagnosticSeverity> severity;
    // The diagnostic's location, made absolute, when it has one.
    std::optional<EditorSourceLocation> location;
};

struct BuildPollResult {
    size_t newLines{0};
    // True on the one poll that observes the build ending.
    bool finished{false};
};

// Runs one build command at a time without blocking its caller, keeping the
// output log and the diagnostics parsed from it. All members are called from
// the owning (UI) thread; ChildProcess does the background reading.
class BuildRunner {
  public:
    // The log keeps the most recent lines only; diagnostics are kept for the
    // whole build.
    static constexpr size_t maximumOutputLines = 20000;

    ChildProcessResult start(const BuildRequest& request);
    // Moves newly read output into the log and parses it.
    BuildPollResult poll();
    void cancel();

    BuildState state() const;
    bool running() const;
    std::optional<int> exitCode() const;
    const std::deque<BuildOutputLine>& output() const;
    // Diagnostics so far, with relative paths made absolute.
    std::vector<CompilerDiagnostic> diagnostics() const;
    const std::string& command() const;
    // How long the last build ran, from start until its end was observed;
    // nullopt while it runs or before the first build.
    std::optional<std::chrono::duration<double>> duration() const;

  private:
    void appendLine(std::string text);
    // Starts the build after a successful prepare step; false (with the
    // reason in the log) when it cannot start.
    bool startPendingBuild();
    EditorSourceLocation resolve(EditorSourceLocation location) const;

    ChildProcess m_process;
    // The build itself while its prepare step runs.
    std::optional<ChildProcessOptions> m_pendingBuild;
    CompilerOutputParser m_parser;
    std::deque<BuildOutputLine> m_output;
    std::string m_command;
    std::filesystem::path m_workingDirectory;
    BuildState m_state{BuildState::Idle};
    std::optional<int> m_exitCode;
    std::chrono::steady_clock::time_point m_startedAt{};
    std::optional<std::chrono::duration<double>> m_duration;
};
