#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// What a child process runs: either a program with an exact argument list,
// which no shell interprets, or a command line handed to the platform shell
// (`cmd.exe /d /s /c` on Windows, `/bin/sh -c` elsewhere).
struct ChildProcessCommand {
    enum class Kind {
        Program,
        Shell
    };

    Kind kind{Kind::Program};
    // Program: the program followed by its arguments, UTF-8. A program
    // without a directory is searched for on PATH (and with PATHEXT on
    // Windows, where `.cmd`/`.bat` programs are run through cmd.exe and reject
    // arguments that cmd would expand). Shell: one element, the command line.
    std::vector<std::string> arguments;

    static ChildProcessCommand program(std::vector<std::string> arguments);
    static ChildProcessCommand shell(std::string commandLine);
};

struct ChildProcessOptions {
    ChildProcessCommand command;
    // Empty inherits the parent's working directory.
    std::filesystem::path workingDirectory;
};

struct ChildProcessResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// One child process with its combined stdout and stderr captured as lines.
//
// A background thread reads output and watches for exit while the process
// runs, so no call here blocks except wait() and destruction. The owning
// thread drives the object; takeLines(), running(), and exitCode() are safe to
// call while the reader thread works. Cancelling or destroying a running
// process terminates it together with the processes it started (a Windows job
// object, a POSIX process group). Descendants that outlive a process which
// exited on its own are terminated on destruction on Windows only; POSIX leaves
// them alone because the reaped group id may be reused. Output is passed through as bytes; line
// endings are normalised and partial final lines are delivered on exit.
class ChildProcess {
  public:
    ChildProcess();
    ~ChildProcess();
    ChildProcess(ChildProcess&&) noexcept;
    ChildProcess& operator=(ChildProcess&&) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts the command. Fails if this object already runs a process.
    ChildProcessResult start(const ChildProcessOptions& options);

    // True from a successful start until the process has exited and all of
    // its output has been read.
    bool running() const;
    // The exit status once finished; a POSIX process ended by a signal
    // reports 128 plus the signal number.
    std::optional<int> exitCode() const;
    bool cancelled() const;
    // Moves out the complete lines read since the last call.
    std::vector<std::string> takeLines();
    // Terminates the process tree; running() turns false once it is reaped.
    void cancel();
    // Blocks until the process has finished.
    void wait();

    // Starts a program that outlives this call: no output is captured, the
    // editor does not wait for it, and it is not terminated with the caller.
    static ChildProcessResult launchDetached(const ChildProcessOptions& options);

  private:
    struct State;
    void finish();

    std::unique_ptr<State> m_state;
};

// This process's id, as debuggers and process tools name it.
uint32_t currentProcessId();
