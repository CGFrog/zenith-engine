#include "engine/platform/ChildProcess.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

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

void testProgramArguments() {
    // Spaces, quotes, and trailing backslashes must reach the child unchanged.
    const Finished echoed = run({.command = cmake({"-E", "echo", "a b", "c\"d", "e\\", "&|<>"})});
    expect(echoed.exitCode == 0, "echo should exit with zero");
    expect(echoed.lines.size() == 1 && echoed.lines.front() == "a b c\"d e\\ &|<>",
           "program arguments should round trip without shell interpretation");
}

void testShellExitCodeAndStreams() {
#ifdef _WIN32
    const std::string command = "echo first&& 1>&2 echo second&& exit /b 3";
#else
    const std::string command = "echo first; echo second 1>&2; exit 3";
#endif
    const Finished finished = run({.command = ChildProcessCommand::shell(command)});
    expect(finished.exitCode == 3, "the shell's exit code should be reported");
    expect(contains(finished.lines, "first"), "stdout should be captured");
    expect(contains(finished.lines, "second"), "stderr should be captured with stdout");
}

void testPartialFinalLine() {
#ifdef _WIN32
    const std::string command = "<nul set /p =partial";
#else
    const std::string command = "printf partial";
#endif
    const Finished finished = run({.command = ChildProcessCommand::shell(command)});
    expect(finished.lines.size() == 1 && finished.lines.front() == "partial",
           "output without a final newline should still be delivered");
}

void testWorkingDirectory() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_child_process_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
#ifdef _WIN32
    const std::string command = "cd";
#else
    const std::string command = "pwd";
#endif
    const Finished finished = run({.command = ChildProcessCommand::shell(command), .workingDirectory = directory});
    expect(finished.lines.size() == 1 &&
               std::filesystem::equivalent(std::filesystem::path(finished.lines.front()), directory, error),
           "the process should start in the requested working directory");
    std::filesystem::remove_all(directory, error);
}

void testCancel() {
    ChildProcess process;
    expect(process.start({.command = cmake({"-E", "sleep", "30"})}).success, "sleep should start");
    expect(process.running(), "a started process should be running");
    expect(!process.start({.command = cmake({"-E", "echo", "x"})}).success,
           "starting while running should be rejected");
    const auto begin = std::chrono::steady_clock::now();
    process.cancel();
    process.wait();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    expect(!process.running() && process.cancelled(), "a cancelled process should stop and report cancellation");
    expect(elapsed < std::chrono::seconds(10), "cancel should terminate promptly");
    expect(process.exitCode().has_value() && process.exitCode() != 0,
           "a cancelled process should report a failure exit code");
}

void testMoveAndRestart() {
    ChildProcess first;
    expect(first.start({.command = cmake({"-E", "echo", "moved"})}).success, "echo should start");
    ChildProcess second = std::move(first);
    expect(!first.running(), "a moved-from process should be empty");
    const Finished finished = collect(second);
    expect(contains(finished.lines, "moved"), "a moved process should keep its output");
    expect(second.start({.command = cmake({"-E", "echo", "again"})}).success,
           "a finished process object should start again");
    expect(contains(collect(second).lines, "again"), "the restarted process should produce its own output");
}

void testFailures() {
    ChildProcess process;
    const ChildProcessResult missing =
        process.start({.command = ChildProcessCommand::program({"zenith-program-that-does-not-exist"})});
    expect(!missing.success && !missing.error.empty(), "a missing program should fail descriptively");
    expect(!process.running(), "a failed start should leave nothing running");
    expect(!process.start({.command = ChildProcessCommand::shell("")}).success, "an empty command should fail");
    expect(!ChildProcess::launchDetached({.command = ChildProcessCommand::program({"zenith-missing-ide"})}).success,
           "a detached launch of a missing program should fail");
    expect(ChildProcess::launchDetached({.command = cmake({"-E", "echo", "detached"})}).success,
           "a detached program should launch");
}
} // namespace

int main() {
    testProgramArguments();
    testShellExitCodeAndStreams();
    testPartialFinalLine();
    testWorkingDirectory();
    testCancel();
    testMoveAndRestart();
    testFailures();
    if (failures != 0) {
        std::cerr << failures << " child process test(s) failed\n";
        return 1;
    }
    return 0;
}
