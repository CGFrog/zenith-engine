#pragma once

// Installs the process's crash handler for its lifetime and restores the previous one when destroyed.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct CrashHandlerOptions {
    // Where the dump and the report are written, created when the handler is
    // installed: the application's diagnostics folder, the one its
    // DiagnosticBundleWriter writes to.
    std::filesystem::path directory;
    // Names the files (`<product>-crash-<UTC time>.dmp` and `.txt`) and is
    // recorded in the report with the version.
    std::string product{"zenith"};
    std::string productVersion;
};

// Something to save before a crashed process ends, such as the editor's
// recovery copy of the open scene. It runs in the crashed process, on a
// helper thread, while the crashing thread waits and every other thread keeps
// running: the heap or the data it reads may be corrupt, so it must be
// best-effort, quick, and touch only state it can reach without locks the
// crashed code may hold. A C++ exception or a further crash inside it is
// caught and recorded in the report.
using CrashRecoveryAction = std::function<void()>;

struct CrashHandlerState;

// Installs the process's crash handler for its lifetime and restores the
// previous one when destroyed. On Windows an unhandled exception (an access
// violation, a stack overflow, or a C++ exception nothing caught) writes a
// minidump and a short text report into options.directory, runs the recovery
// action, writes a line to stderr naming the report, and ends the process
// with the exception code (no error dialog, so automated runs do not hang).
// The handler is process-wide, so only one scope can be installed at a time;
// a second one is inert, as is one without a directory. Elsewhere the scope
// installs nothing (installed() is false). Not called while a debugger is
// attached, which sees the exception first.
class CrashHandlerScope {
  public:
    explicit CrashHandlerScope(CrashHandlerOptions options);
    ~CrashHandlerScope();

    CrashHandlerScope(const CrashHandlerScope&) = delete;
    CrashHandlerScope& operator=(const CrashHandlerScope&) = delete;

    // Whether this scope's handler is the process's crash handler.
    bool installed() const;
    const std::filesystem::path& directory() const;
    // Replaces the recovery action; an empty one removes it. The owner of
    // what the action refers to removes it before destroying that. Thread
    // safe.
    void setRecoveryAction(CrashRecoveryAction action);

  private:
    std::unique_ptr<CrashHandlerState> m_state;
};
