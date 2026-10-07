#include "engine/platform/ChildProcess.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

TEST_SUITE_BEGIN("ChildProcess");

namespace {

struct Finished {
    std::vector<std::string> lines;
    std::optional<int> exitCode;
};

// Collects lines while the process runs, as the editor does each frame.
Finished collect(ChildProcess& process) {
    Finished finished;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (process.running() && std::chrono::steady_clock::now() < deadline) {
        std::vector<std::string> lines = process.takeLines();
        finished.lines.insert(finished.lines.end(), lines.begin(), lines.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    process.wait();
    std::vector<std::string> lines = process.takeLines();
    finished.lines.insert(finished.lines.end(), lines.begin(), lines.end());
    finished.exitCode = process.exitCode();
    return finished;
}

Finished run(const ChildProcessOptions& options) {
    ChildProcess process;
    const ChildProcessResult started = process.start(options);
    if (!started) {
        std::cerr << "start failed: " << started.error << '\n';
        return {};
    }
    return collect(process);
}

bool contains(const std::vector<std::string>& lines, const std::string& line) {
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

ChildProcessCommand cmake(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), ZENITH_TEST_CMAKE);
    return ChildProcessCommand::program(std::move(arguments));
}

TEST_CASE("ChildProcess: program arguments") {
    // Spaces, quotes, and trailing backslashes must reach the child unchanged.
    const Finished echoed = run({.command = cmake({"-E", "echo", "a b", "c\"d", "e\\", "&|<>"})});
    CHECK_MESSAGE(echoed.exitCode == 0, "echo should exit with zero");
    CHECK_MESSAGE((echoed.lines.size() == 1 && echoed.lines.front() == "a b c\"d e\\ &|<>"),
                  "program arguments should round trip without shell interpretation");
}

TEST_CASE("ChildProcess: shell exit code and streams") {
#ifdef _WIN32
    const std::string command = "echo first&& 1>&2 echo second&& exit /b 3";
#else
    const std::string command = "echo first; echo second 1>&2; exit 3";
#endif
    const Finished finished = run({.command = ChildProcessCommand::shell(command)});
    CHECK_MESSAGE(finished.exitCode == 3, "the shell's exit code should be reported");
    CHECK_MESSAGE(contains(finished.lines, "first"), "stdout should be captured");
    CHECK_MESSAGE(contains(finished.lines, "second"), "stderr should be captured with stdout");
}

TEST_CASE("ChildProcess: partial final line") {
#ifdef _WIN32
    const std::string command = "<nul set /p =partial";
#else
    const std::string command = "printf partial";
#endif
    const Finished finished = run({.command = ChildProcessCommand::shell(command)});
    CHECK_MESSAGE((finished.lines.size() == 1 && finished.lines.front() == "partial"),
                  "output without a final newline should still be delivered");
}

void setParentVariable(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

#ifdef _WIN32
// Only the Windows case-insensitivity check needs it.
bool startsWithAny(const std::vector<std::string>& lines, const std::string& prefix) {
    return std::ranges::any_of(lines, [&prefix](const std::string& line) { return line.starts_with(prefix); });
}
#endif

TEST_CASE("ChildProcess: environment") {
    setParentVariable("ZENITH_CHILD_REPLACED", "parent");
    setParentVariable("ZENITH_CHILD_INHERITED", "kept");
    const Finished printed =
        run({.command = cmake({"-E", "environment"}),
             .environment = {{"ZENITH_CHILD_REPLACED", "child value"}, {"ZENITH_CHILD_ADDED", "added"}}});
    CHECK_MESSAGE(printed.exitCode == 0, "printing the environment should succeed");
    CHECK_MESSAGE((contains(printed.lines, "ZENITH_CHILD_REPLACED=child value") &&
                   !contains(printed.lines, "ZENITH_CHILD_REPLACED=parent")),
                  "an override replaces the parent's variable");
    CHECK_MESSAGE(contains(printed.lines, "ZENITH_CHILD_ADDED=added"), "an override adds a variable");
    CHECK_MESSAGE(contains(printed.lines, "ZENITH_CHILD_INHERITED=kept"), "the rest of the environment is inherited");
#ifdef _WIN32
    const Finished cased =
        run({.command = cmake({"-E", "environment"}), .environment = {{"zenith_child_replaced", "lower"}}});
    CHECK_MESSAGE(
        (contains(cased.lines, "zenith_child_replaced=lower") && !startsWithAny(cased.lines, "ZENITH_CHILD_REPLACED=")),
        "Windows compares variable names without case");
#endif

    // A bare program name is found on the overridden PATH.
    const std::string cmakeDirectory = std::filesystem::path(ZENITH_TEST_CMAKE).parent_path().string();
    const Finished found = run({.command = ChildProcessCommand::program({"cmake", "-E", "echo", "found"}),
                                .environment = {{"PATH", cmakeDirectory}}});
    CHECK_MESSAGE((found.exitCode == 0 && contains(found.lines, "found")),
                  "a program is searched for on the overridden PATH");
}

TEST_CASE("ChildProcess: working directory") {
    const testsupport::TempDirectory scratch("child-process-tests");
    const std::filesystem::path directory = scratch.path();
    std::error_code error;
#ifdef _WIN32
    const std::string command = "cd";
#else
    const std::string command = "pwd";
#endif
    const Finished finished = run({.command = ChildProcessCommand::shell(command), .workingDirectory = directory});
    CHECK_MESSAGE((finished.lines.size() == 1 &&
                   std::filesystem::equivalent(std::filesystem::path(finished.lines.front()), directory, error)),
                  "the process should start in the requested working directory");
}

TEST_CASE("ChildProcess: cancel") {
    ChildProcess process;
    CHECK_MESSAGE(process.start({.command = cmake({"-E", "sleep", "30"})}).success, "sleep should start");
    CHECK_MESSAGE(process.running(), "a started process should be running");
    CHECK_MESSAGE(!process.start({.command = cmake({"-E", "echo", "x"})}).success,
                  "starting while running should be rejected");
    const auto begin = std::chrono::steady_clock::now();
    process.cancel();
    process.wait();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    CHECK_MESSAGE((!process.running() && process.cancelled()),
                  "a cancelled process should stop and report cancellation");
    CHECK_MESSAGE(elapsed < std::chrono::seconds(10), "cancel should terminate promptly");
    CHECK_MESSAGE((process.exitCode().has_value() && process.exitCode() != 0),
                  "a cancelled process should report a failure exit code");
}

TEST_CASE("ChildProcess: move and restart") {
    ChildProcess first;
    CHECK_MESSAGE(first.start({.command = cmake({"-E", "echo", "moved"})}).success, "echo should start");
    ChildProcess second = std::move(first);
    CHECK_MESSAGE(!first.running(), "a moved-from process should be empty");
    const Finished finished = collect(second);
    CHECK_MESSAGE(contains(finished.lines, "moved"), "a moved process should keep its output");
    CHECK_MESSAGE(second.start({.command = cmake({"-E", "echo", "again"})}).success,
                  "a finished process object should start again");
    CHECK_MESSAGE(contains(collect(second).lines, "again"), "the restarted process should produce its own output");
}

TEST_CASE("ChildProcess: failures") {
    ChildProcess process;
    const ChildProcessResult missing =
        process.start({.command = ChildProcessCommand::program({"zenith-program-that-does-not-exist"})});
    CHECK_MESSAGE((!missing.success && !missing.error.empty()), "a missing program should fail descriptively");
    CHECK_MESSAGE(!process.running(), "a failed start should leave nothing running");
    CHECK_MESSAGE(!process.start({.command = ChildProcessCommand::shell("")}).success, "an empty command should fail");
    CHECK_MESSAGE(
        !ChildProcess::launchDetached({.command = ChildProcessCommand::program({"zenith-missing-ide"})}).success,
        "a detached launch of a missing program should fail");
    CHECK_MESSAGE(ChildProcess::launchDetached({.command = cmake({"-E", "echo", "detached"})}).success,
                  "a detached program should launch");
}
} // namespace

TEST_SUITE_END();
