#include "engine/platform/CrashHandler.hpp"

#include <mutex>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#endif

struct CrashHandlerState {
    std::filesystem::path directory;
    std::string product;
    std::string productVersion;
    std::mutex recoveryMutex;
    CrashRecoveryAction recovery;
    bool installed{false};
#ifdef _WIN32
    // Converted at install: the crash path avoids allocating where it can.
    std::wstring directoryText;
    std::wstring productText;
    LPTOP_LEVEL_EXCEPTION_FILTER previousFilter{nullptr};
#endif
};

#ifdef _WIN32
namespace {
// The process-wide handler can only reach its scope through a global.
std::atomic<CrashHandlerState*> activeState{nullptr};
// Set by the first crash, so a crash while handling one is left to Windows.
std::atomic<bool> crashing{false};

constexpr size_t pathCapacity = 1024;
// The crash work runs on its own thread: the crashing thread's stack may be
// exhausted (a stack overflow), and MiniDumpWriteDump is best called from
// another thread. A generous reservation for the recovery action.
constexpr SIZE_T crashThreadStackBytes = 8u * 1024u * 1024u;
// A recovery action that hangs must not keep a crashed process alive.
constexpr DWORD crashWorkTimeoutMilliseconds = 60'000;

struct CrashWork {
    CrashHandlerState* state{nullptr};
    EXCEPTION_POINTERS* exception{nullptr};
    DWORD threadId{0};
};

enum class RecoveryOutcome {
    None,
    Finished,
    Threw,
    Busy,
};

struct RecoveryCall {
    CrashHandlerState* state{nullptr};
    RecoveryOutcome outcome{RecoveryOutcome::None};
};

const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer division by zero";
    case EXCEPTION_IN_PAGE_ERROR:
        return "in-page error";
    case 0xE06D7363:
        return "uncaught C++ exception";
    default:
        return "unhandled exception";
    }
}

void writeText(HANDLE file, const char* text) {
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

bool writeMinidump(const wchar_t* path, const CrashWork& work) {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = work.threadId;
    exception.ExceptionPointers = work.exception;
    exception.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules);
    const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                           work.exception != nullptr ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return written != FALSE;
}

// Runs the recovery action unless the owner is replacing it right now.
void runRecovery(void* argument) {
    auto& call = *static_cast<RecoveryCall*>(argument);
    std::unique_lock lock(call.state->recoveryMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        call.outcome = RecoveryOutcome::Busy;
        return;
    }
    if (!call.state->recovery) {
        return;
    }
    try {
        call.state->recovery();
        call.outcome = RecoveryOutcome::Finished;
    } catch (...) {
        call.outcome = RecoveryOutcome::Threw;
    }
}

// Calls function, returning false when it crashed. Holds no C++ objects, as
// a function with __try must not.
bool callShielded(void (*function)(void*), void* argument) {
    __try {
        function(argument);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* recoveryText(bool survived, RecoveryOutcome outcome) {
    if (!survived) {
        return "recovery: the recovery action crashed\r\n";
    }
    switch (outcome) {
    case RecoveryOutcome::None:
        return "recovery: none registered\r\n";
    case RecoveryOutcome::Finished:
        return "recovery: finished\r\n";
    case RecoveryOutcome::Threw:
        return "recovery: the recovery action threw\r\n";
    case RecoveryOutcome::Busy:
        return "recovery: skipped, it was being replaced\r\n";
    }
    return "\r\n";
}

DWORD WINAPI writeCrash(LPVOID argument) {
    const CrashWork& work = *static_cast<const CrashWork*>(argument);
    CrashHandlerState& state = *work.state;

    SYSTEMTIME time{};
    GetSystemTime(&time);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02uT%02u%02u%02uZ", time.wYear, time.wMonth, time.wDay, time.wHour,
                  time.wMinute, time.wSecond);
    wchar_t dumpPath[pathCapacity];
    wchar_t reportPath[pathCapacity];
    std::swprintf(dumpPath, pathCapacity, L"%ls\\%ls-crash-%hs.dmp", state.directoryText.c_str(),
                  state.productText.c_str(), stamp);
    std::swprintf(reportPath, pathCapacity, L"%ls\\%ls-crash-%hs.txt", state.directoryText.c_str(),
                  state.productText.c_str(), stamp);

    const bool dumped = writeMinidump(dumpPath, work);

    const EXCEPTION_RECORD* record = work.exception != nullptr ? work.exception->ExceptionRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    void* address = record != nullptr ? record->ExceptionAddress : nullptr;
    char module[MAX_PATH] = "an unknown module";
    HMODULE moduleHandle = nullptr;
    if (address != nullptr &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &moduleHandle) != FALSE) {
        GetModuleFileNameA(moduleHandle, module, MAX_PATH);
    }
    char text[2048];
    std::snprintf(text, sizeof(text),
                  "%s %s crashed\r\n"
                  "time (UTC): %s\r\n"
                  "exception: 0x%08lX (%s)\r\n"
                  "address: %p in %s\r\n"
                  "thread: %lu\r\n"
                  "minidump: %s\r\n",
                  state.product.c_str(), state.productVersion.c_str(), stamp, static_cast<unsigned long>(code),
                  exceptionName(code), address, module, static_cast<unsigned long>(work.threadId),
                  dumped ? "written beside this report" : "could not be written");
    // The report exists before the recovery action runs, in case it hangs.
    const HANDLE report =
        CreateFileW(reportPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    writeText(report, text);
    FlushFileBuffers(report);

    RecoveryCall recovery{.state = &state};
    const bool survived = callShielded(runRecovery, &recovery);
    writeText(report, recoveryText(survived, recovery.outcome));
    if (report != INVALID_HANDLE_VALUE) {
        CloseHandle(report);
    }

    char reportName[pathCapacity];
    if (WideCharToMultiByte(CP_UTF8, 0, reportPath, -1, reportName, static_cast<int>(sizeof(reportName)), nullptr,
                            nullptr) == 0) {
        std::snprintf(reportName, sizeof(reportName), "the diagnostics folder");
    }
    std::snprintf(text, sizeof(text), "%s crashed (exception 0x%08lX, %s). Crash report: %s\n", state.product.c_str(),
                  static_cast<unsigned long>(code), exceptionName(code), reportName);
    writeText(GetStdHandle(STD_ERROR_HANDLE), text);
    return 0;
}

LONG WINAPI handleUnhandledException(EXCEPTION_POINTERS* exception) {
    CrashHandlerState* state = activeState.load();
    if (state == nullptr || crashing.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CrashWork work{.state = state, .exception = exception, .threadId = GetCurrentThreadId()};
    if (const HANDLE thread = CreateThread(nullptr, crashThreadStackBytes, writeCrash, &work,
                                           STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
        WaitForSingleObject(thread, crashWorkTimeoutMilliseconds);
        CloseHandle(thread);
    } else {
        writeCrash(&work);
    }
    // Ends the process with the exception code, without an error dialog.
    return EXCEPTION_EXECUTE_HANDLER;
}

std::wstring wideText(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())).wstring();
}
} // namespace
#endif

CrashHandlerScope::CrashHandlerScope(CrashHandlerOptions options)
    : m_state(std::make_unique<CrashHandlerState>()) {
    m_state->directory = std::move(options.directory);
    m_state->product = options.product.empty() ? std::string("zenith") : std::move(options.product);
    m_state->productVersion = std::move(options.productVersion);
    if (m_state->directory.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(m_state->directory, error);
#ifdef _WIN32
    m_state->directoryText = m_state->directory.wstring();
    m_state->productText = wideText(m_state->product);
    CrashHandlerState* expected = nullptr;
    if (activeState.compare_exchange_strong(expected, m_state.get())) {
        m_state->previousFilter = SetUnhandledExceptionFilter(handleUnhandledException);
        m_state->installed = true;
    }
#endif
}

CrashHandlerScope::~CrashHandlerScope() {
#ifdef _WIN32
    if (m_state->installed) {
        SetUnhandledExceptionFilter(m_state->previousFilter);
        activeState.store(nullptr);
    }
#endif
}

bool CrashHandlerScope::installed() const {
    return m_state->installed;
}

const std::filesystem::path& CrashHandlerScope::directory() const {
    return m_state->directory;
}

void CrashHandlerScope::setRecoveryAction(CrashRecoveryAction action) {
    const std::lock_guard lock(m_state->recoveryMutex);
    m_state->recovery = std::move(action);
}
