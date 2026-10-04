#include "ChildProcess.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwctype>
#else
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr size_t readChunkBytes = 4096;

// Splits a byte stream into lines. Only the reader thread touches it.
class LineSplitter {
  public:
    void append(std::string_view bytes, std::vector<std::string>& completeLines) {
        for (const char character : bytes) {
            if (character == '\n') {
                completeLines.push_back(takePartial());
            } else {
                m_partial.push_back(character);
            }
        }
    }

    std::optional<std::string> flush() {
        if (m_partial.empty()) {
            return std::nullopt;
        }
        return takePartial();
    }

  private:
    std::string takePartial() {
        if (!m_partial.empty() && m_partial.back() == '\r') {
            m_partial.pop_back();
        }
        return std::exchange(m_partial, {});
    }

    std::string m_partial;
};

ChildProcessResult failure(std::string error) {
    return {.success = false, .error = std::move(error)};
}

ChildProcessResult validateCommand(const ChildProcessCommand& command) {
    if (command.arguments.empty() || command.arguments.front().empty()) {
        return failure(command.kind == ChildProcessCommand::Kind::Shell ? "The command line is empty"
                                                                        : "No program was given to run");
    }
    if (command.kind == ChildProcessCommand::Kind::Shell && command.arguments.size() != 1) {
        return failure("A shell command must be a single command line");
    }
    for (const std::string& argument : command.arguments) {
        if (argument.find('\0') != std::string::npos) {
            return failure("Command arguments cannot contain NUL characters");
        }
    }
    return {.success = true};
}
} // namespace

ChildProcessCommand ChildProcessCommand::program(std::vector<std::string> arguments) {
    return {.kind = Kind::Program, .arguments = std::move(arguments)};
}

ChildProcessCommand ChildProcessCommand::shell(std::string commandLine) {
    return {.kind = Kind::Shell, .arguments = {std::move(commandLine)}};
}

#ifdef _WIN32
namespace {
std::string lastErrorMessage(std::string_view operation) {
    const DWORD code = GetLastError();
    return std::string(operation) + ": " + std::system_category().message(static_cast<int>(code));
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::wstring environmentVariable(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return {};
    }
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    value.resize(written);
    return value;
}

std::filesystem::path commandInterpreter() {
    std::array<wchar_t, MAX_PATH> directory{};
    const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) {
        return L"cmd.exe";
    }
    return std::filesystem::path(std::wstring(directory.data(), length)) / L"cmd.exe";
}

// Quotes one argument so CommandLineToArgvW and the C runtime recover it
// exactly.
std::wstring quoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    for (auto character = argument.begin();; ++character) {
        size_t backslashes = 0;
        while (character != argument.end() && *character == L'\\') {
            ++character;
            ++backslashes;
        }
        if (character == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(*character);
    }
    quoted.push_back(L'"');
    return quoted;
}

// Finds a program the way a console user would: explicit paths as given,
// bare names on PATH with each PATHEXT extension. The current directory is
// not searched.
std::optional<std::filesystem::path> resolveProgram(const std::wstring& program) {
    const std::filesystem::path candidate(program);
    if (candidate.has_parent_path()) {
        return candidate;
    }
    const std::wstring searchPath = environmentVariable(L"PATH");
    std::vector<std::wstring> extensions;
    if (candidate.has_extension()) {
        extensions.emplace_back();
    }
    std::wstring pathExtensions = environmentVariable(L"PATHEXT");
    if (pathExtensions.empty()) {
        pathExtensions = L".COM;.EXE;.BAT;.CMD";
    }
    size_t begin = 0;
    while (begin <= pathExtensions.size()) {
        const size_t end = std::min(pathExtensions.find(L';', begin), pathExtensions.size());
        if (end > begin) {
            extensions.push_back(pathExtensions.substr(begin, end - begin));
        }
        begin = end + 1;
    }
    for (const std::wstring& extension : extensions) {
        std::array<wchar_t, 32768> resolved{};
        const DWORD length = SearchPathW(searchPath.empty() ? nullptr : searchPath.c_str(), program.c_str(),
                                         extension.empty() ? nullptr : extension.c_str(),
                                         static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
        if (length > 0 && length < resolved.size()) {
            return std::filesystem::path(std::wstring(resolved.data(), length));
        }
    }
    return std::nullopt;
}

bool isBatchFile(const std::filesystem::path& program) {
    std::wstring extension = program.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
    return extension == L".cmd" || extension == L".bat";
}

struct WindowsCommandLine {
    std::wstring application;
    std::wstring commandLine;
};

struct WindowsCommandLineResult {
    std::optional<WindowsCommandLine> command;
    std::string error;
};

WindowsCommandLineResult buildCommandLine(const ChildProcessCommand& command) {
    if (command.kind == ChildProcessCommand::Kind::Shell) {
        const std::wstring interpreter = commandInterpreter().wstring();
        // /s strips exactly the outer quotes, so the command reaches cmd as typed.
        return {.command = WindowsCommandLine{interpreter, quoteArgument(interpreter) + L" /d /s /c \"" +
                                                               widen(command.arguments.front()) + L"\""}};
    }

    const std::optional<std::filesystem::path> program = resolveProgram(widen(command.arguments.front()));
    if (!program) {
        return {.error = "Could not find program on PATH: " + command.arguments.front()};
    }
    if (!isBatchFile(*program)) {
        std::wstring commandLine = quoteArgument(program->wstring());
        for (size_t index = 1; index < command.arguments.size(); ++index) {
            commandLine += L' ';
            commandLine += quoteArgument(widen(command.arguments[index]));
        }
        return {.command = WindowsCommandLine{program->wstring(), std::move(commandLine)}};
    }

    // Batch files only run through cmd.exe, which expands % and treats quotes
    // as toggles even inside arguments. Reject what cannot be passed safely.
    for (const std::string& argument : command.arguments) {
        if (argument.find_first_of("\"%\r\n") != std::string::npos) {
            return {.error =
                        "Arguments to a batch file cannot contain quotes, percent signs, or line breaks: " + argument};
        }
    }
    const std::wstring interpreter = commandInterpreter().wstring();
    std::wstring inner = L"\"" + program->wstring() + L"\"";
    for (size_t index = 1; index < command.arguments.size(); ++index) {
        inner += L" \"" + widen(command.arguments[index]) + L"\"";
    }
    return {.command =
                WindowsCommandLine{interpreter, quoteArgument(interpreter) + L" /d /v:off /s /c \"" + inner + L"\""}};
}

struct NativeProcess {
    HANDLE process{nullptr};
    HANDLE job{nullptr};
    HANDLE outputRead{nullptr};
};

void closeHandle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}
} // namespace

struct ChildProcess::State {
    std::mutex mutex;
    std::vector<std::string> lines;
    std::optional<int> exitCode;
    std::atomic<bool> finished{false};
    std::atomic<bool> cancelled{false};
    std::thread reader;
    NativeProcess native;

    void deliver(std::vector<std::string>& completeLines) {
        if (completeLines.empty()) {
            return;
        }
        const std::lock_guard lock(mutex);
        std::move(completeLines.begin(), completeLines.end(), std::back_inserter(lines));
        completeLines.clear();
    }

    // Reads whatever is buffered in the pipe without blocking. Returns false
    // when nothing was read.
    bool readAvailable(LineSplitter& splitter) {
        DWORD available = 0;
        if (!PeekNamedPipe(native.outputRead, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            return false;
        }
        std::array<char, readChunkBytes> buffer{};
        DWORD bytesRead = 0;
        const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(native.outputRead, buffer.data(), request, &bytesRead, nullptr) || bytesRead == 0) {
            return false;
        }
        std::vector<std::string> completeLines;
        splitter.append(std::string_view(buffer.data(), bytesRead), completeLines);
        deliver(completeLines);
        return true;
    }

    // Polls rather than blocking on the pipe: a descendant that inherited the
    // pipe can hold it open after the process itself exits.
    void readUntilExit() {
        LineSplitter splitter;
        for (;;) {
            if (readAvailable(splitter)) {
                continue;
            }
            if (WaitForSingleObject(native.process, 10) == WAIT_OBJECT_0) {
                while (readAvailable(splitter)) {
                }
                break;
            }
        }
        std::vector<std::string> remaining;
        if (std::optional<std::string> last = splitter.flush()) {
            remaining.push_back(std::move(*last));
        }
        deliver(remaining);
        DWORD code = 0;
        GetExitCodeProcess(native.process, &code);
        {
            const std::lock_guard lock(mutex);
            exitCode = static_cast<int>(code);
        }
        finished = true;
    }

    void terminate() {
        if (native.job != nullptr) {
            TerminateJobObject(native.job, 1);
        } else if (native.process != nullptr) {
            TerminateProcess(native.process, 1);
        }
    }

    void release() {
        closeHandle(native.outputRead);
        closeHandle(native.process);
        // The job kills any descendants still alive when it closes.
        closeHandle(native.job);
    }
};

ChildProcessResult ChildProcess::start(const ChildProcessOptions& options) {
    if (running()) {
        return failure("A process is already running");
    }
    if (const ChildProcessResult valid = validateCommand(options.command); !valid) {
        return valid;
    }
    const WindowsCommandLineResult built = buildCommandLine(options.command);
    if (!built.command) {
        return failure(built.error);
    }
    finish();
    auto state = std::make_unique<State>();

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&state->native.outputRead, &outputWrite, &inheritable, 0)) {
        return failure(lastErrorMessage("Could not create output pipe"));
    }
    SetHandleInformation(state->native.outputRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    auto closeChildEnds = [&] {
        closeHandle(outputWrite);
        closeHandle(input);
    };
    if (input == INVALID_HANDLE_VALUE) {
        closeChildEnds();
        state->release();
        return failure(lastErrorMessage("Could not open the null device"));
    }

    // Restrict inheritance to this child's two handles so concurrent launches
    // never receive each other's pipes.
    std::array<HANDLE, 2> inherited{outputWrite, input};
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   sizeof(inherited), nullptr, nullptr)) {
        closeChildEnds();
        state->release();
        return failure(lastErrorMessage("Could not prepare process attributes"));
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = outputWrite;
    startup.StartupInfo.hStdError = outputWrite;
    startup.lpAttributeList = attributes;

    std::wstring commandLine = built.command->commandLine;
    const std::wstring workingDirectory = options.workingDirectory.wstring();
    PROCESS_INFORMATION information{};
    const BOOL created = CreateProcessW(built.command->application.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                        &startup.StartupInfo, &information);
    const std::string createError = created ? std::string{} : lastErrorMessage("Could not start process");
    DeleteProcThreadAttributeList(attributes);
    closeChildEnds();
    if (!created) {
        state->release();
        return failure(createError);
    }

    // Join the job before the process can start descendants of its own.
    state->native.process = information.hProcess;
    state->native.job = CreateJobObjectW(nullptr, nullptr);
    if (state->native.job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(state->native.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(state->native.job, information.hProcess)) {
            closeHandle(state->native.job);
        }
    }
    ResumeThread(information.hThread);
    CloseHandle(information.hThread);

    State* reading = state.get();
    state->reader = std::thread([reading] { reading->readUntilExit(); });
    m_state = std::move(state);
    return {.success = true};
}

ChildProcessResult ChildProcess::launchDetached(const ChildProcessOptions& options) {
    if (const ChildProcessResult valid = validateCommand(options.command); !valid) {
        return valid;
    }
    const WindowsCommandLineResult built = buildCommandLine(options.command);
    if (!built.command) {
        return failure(built.error);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring commandLine = built.command->commandLine;
    const std::wstring workingDirectory = options.workingDirectory.wstring();
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(built.command->application.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &information)) {
        return failure(lastErrorMessage("Could not start process"));
    }
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
    return {.success = true};
}

#else

namespace {
std::string errnoMessage(std::string_view operation, int code) {
    return std::string(operation) + ": " + std::strerror(code);
}

std::optional<std::string> resolveProgram(const std::string& program) {
    if (program.find('/') != std::string::npos) {
        return program;
    }
    const char* path = std::getenv("PATH");
    const std::string_view directories = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t begin = 0;
    while (begin <= directories.size()) {
        const size_t end = std::min(directories.find(':', begin), directories.size());
        const std::string directory(directories.substr(begin, end - begin));
        const std::string candidate = (directory.empty() ? std::string(".") : directory) + "/" + program;
        struct stat status{};
        if (stat(candidate.c_str(), &status) == 0 && S_ISREG(status.st_mode) && access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
        begin = end + 1;
    }
    return std::nullopt;
}

// Owned argv storage prepared before fork, because only async-signal-safe
// calls may run in the child.
struct ExecArguments {
    std::string program;
    std::vector<std::string> storage;
    std::vector<char*> pointers;
};

struct ExecArgumentsResult {
    std::optional<ExecArguments> arguments;
    std::string error;
};

ExecArgumentsResult buildExecArguments(const ChildProcessCommand& command) {
    ExecArguments built;
    if (command.kind == ChildProcessCommand::Kind::Shell) {
        built.program = "/bin/sh";
        built.storage = {"/bin/sh", "-c", command.arguments.front()};
    } else {
        std::optional<std::string> program = resolveProgram(command.arguments.front());
        if (!program) {
            return {.error = "Could not find program on PATH: " + command.arguments.front()};
        }
        built.program = std::move(*program);
        built.storage = command.arguments;
    }
    for (std::string& argument : built.storage) {
        built.pointers.push_back(argument.data());
    }
    built.pointers.push_back(nullptr);
    return {.arguments = std::move(built)};
}

bool setCloseOnExec(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFD);
    return flags != -1 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != -1;
}

struct Pipe {
    int read{-1};
    int write{-1};

    bool open() {
        std::array<int, 2> descriptors{-1, -1};
#ifdef __APPLE__
        // No pipe2 here; a concurrent fork can briefly inherit these ends.
        if (pipe(descriptors.data()) != 0) {
            return false;
        }
        read = descriptors[0];
        write = descriptors[1];
        return setCloseOnExec(read) && setCloseOnExec(write);
#else
        if (pipe2(descriptors.data(), O_CLOEXEC) != 0) {
            return false;
        }
        read = descriptors[0];
        write = descriptors[1];
        return true;
#endif
    }

    void closeRead() {
        if (read != -1) {
            close(read);
            read = -1;
        }
    }

    void closeWrite() {
        if (write != -1) {
            close(write);
            write = -1;
        }
    }

    void closeBoth() {
        closeRead();
        closeWrite();
    }
};

// Runs in the forked child: redirects standard streams and executes. On
// failure it reports errno through the close-on-exec error pipe.
[[noreturn]] void execChild(const ExecArguments& arguments, const char* workingDirectory, int outputDescriptor,
                            int errorDescriptor) {
    const int input = ::open("/dev/null", O_RDONLY);
    if (input != -1) {
        dup2(input, STDIN_FILENO);
    }
    if (outputDescriptor != -1) {
        dup2(outputDescriptor, STDOUT_FILENO);
        dup2(outputDescriptor, STDERR_FILENO);
    } else {
        const int output = ::open("/dev/null", O_WRONLY);
        if (output != -1) {
            dup2(output, STDOUT_FILENO);
            dup2(output, STDERR_FILENO);
        }
    }
    if (workingDirectory != nullptr && chdir(workingDirectory) != 0) {
        const int code = errno;
        [[maybe_unused]] const ssize_t written = ::write(errorDescriptor, &code, sizeof(code));
        _exit(127);
    }
    execv(arguments.program.c_str(), arguments.pointers.data());
    const int code = errno;
    [[maybe_unused]] const ssize_t written = ::write(errorDescriptor, &code, sizeof(code));
    _exit(127);
}

// Reads the exec status from the error pipe: end of file means exec
// succeeded and closed the pipe.
std::optional<int> readExecError(int descriptor) {
    int code = 0;
    ssize_t bytes = 0;
    do {
        bytes = ::read(descriptor, &code, sizeof(code));
    } while (bytes == -1 && errno == EINTR);
    if (bytes == static_cast<ssize_t>(sizeof(code))) {
        return code;
    }
    return std::nullopt;
}

int exitStatusCode(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

struct NativeProcess {
    pid_t pid{-1};
    int outputRead{-1};
    // Guards `reaped` so cancel never signals a recycled process group.
    std::mutex reapMutex;
    bool reaped{false};
};
} // namespace

struct ChildProcess::State {
    std::mutex mutex;
    std::vector<std::string> lines;
    std::optional<int> exitCode;
    std::atomic<bool> finished{false};
    std::atomic<bool> cancelled{false};
    std::thread reader;
    NativeProcess native;

    void deliver(std::vector<std::string>& completeLines) {
        if (completeLines.empty()) {
            return;
        }
        const std::lock_guard lock(mutex);
        std::move(completeLines.begin(), completeLines.end(), std::back_inserter(lines));
        completeLines.clear();
    }

    // Reads until the non-blocking pipe is empty. Returns false at end of
    // file.
    bool readAvailable(LineSplitter& splitter) {
        std::array<char, readChunkBytes> buffer{};
        for (;;) {
            const ssize_t bytes = ::read(native.outputRead, buffer.data(), buffer.size());
            if (bytes > 0) {
                std::vector<std::string> completeLines;
                splitter.append(std::string_view(buffer.data(), static_cast<size_t>(bytes)), completeLines);
                deliver(completeLines);
                continue;
            }
            if (bytes == -1 && errno == EINTR) {
                continue;
            }
            return bytes != 0;
        }
    }

    std::optional<int> tryReap() {
        const std::lock_guard lock(native.reapMutex);
        int status = 0;
        const pid_t result = waitpid(native.pid, &status, WNOHANG);
        if (result == native.pid) {
            native.reaped = true;
            return exitStatusCode(status);
        }
        if (result == -1 && errno != EINTR) {
            native.reaped = true;
            return -1;
        }
        return std::nullopt;
    }

    // Polls rather than waiting for end of file: a descendant that inherited
    // the pipe can hold it open after the process itself exits.
    void readUntilExit() {
        LineSplitter splitter;
        bool open = true;
        std::optional<int> code;
        while (!code) {
            if (open) {
                pollfd descriptor{native.outputRead, POLLIN, 0};
                poll(&descriptor, 1, 10);
                open = readAvailable(splitter);
            } else {
                usleep(10000);
            }
            code = tryReap();
        }
        if (open) {
            readAvailable(splitter);
        }
        std::vector<std::string> remaining;
        if (std::optional<std::string> last = splitter.flush()) {
            remaining.push_back(std::move(*last));
        }
        deliver(remaining);
        {
            const std::lock_guard lock(mutex);
            exitCode = *code;
        }
        finished = true;
    }

    void terminate() {
        const std::lock_guard lock(native.reapMutex);
        if (native.pid > 0 && !native.reaped) {
            kill(-native.pid, SIGKILL);
            kill(native.pid, SIGKILL);
        }
    }

    void release() {
        if (native.outputRead != -1) {
            close(native.outputRead);
            native.outputRead = -1;
        }
    }
};

ChildProcessResult ChildProcess::start(const ChildProcessOptions& options) {
    if (running()) {
        return failure("A process is already running");
    }
    if (const ChildProcessResult valid = validateCommand(options.command); !valid) {
        return valid;
    }
    const ExecArgumentsResult built = buildExecArguments(options.command);
    if (!built.arguments) {
        return failure(built.error);
    }
    finish();
    const std::string workingDirectory = options.workingDirectory.string();

    Pipe output;
    Pipe execError;
    if (!output.open() || !execError.open()) {
        const int code = errno;
        output.closeBoth();
        execError.closeBoth();
        return failure(errnoMessage("Could not create output pipe", code));
    }
    const pid_t pid = fork();
    if (pid == -1) {
        const int code = errno;
        output.closeBoth();
        execError.closeBoth();
        return failure(errnoMessage("Could not start process", code));
    }
    if (pid == 0) {
        // Own process group, so cancel reaches every descendant.
        setpgid(0, 0);
        execChild(*built.arguments, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), output.write,
                  execError.write);
    }
    setpgid(pid, pid);
    output.closeWrite();
    execError.closeWrite();
    const std::optional<int> execFailure = readExecError(execError.read);
    execError.closeRead();
    if (execFailure) {
        output.closeRead();
        int status = 0;
        waitpid(pid, &status, 0);
        return failure(errnoMessage("Could not start process", *execFailure));
    }

    fcntl(output.read, F_SETFL, fcntl(output.read, F_GETFL) | O_NONBLOCK);
    auto state = std::make_unique<State>();
    state->native.pid = pid;
    state->native.outputRead = output.read;
    State* reading = state.get();
    state->reader = std::thread([reading] { reading->readUntilExit(); });
    m_state = std::move(state);
    return {.success = true};
}

ChildProcessResult ChildProcess::launchDetached(const ChildProcessOptions& options) {
    if (const ChildProcessResult valid = validateCommand(options.command); !valid) {
        return valid;
    }
    const ExecArgumentsResult built = buildExecArguments(options.command);
    if (!built.arguments) {
        return failure(built.error);
    }
    const std::string workingDirectory = options.workingDirectory.string();
    Pipe execError;
    if (!execError.open()) {
        const int code = errno;
        execError.closeBoth();
        return failure(errnoMessage("Could not create pipe", code));
    }
    // Double fork: the grandchild is re-parented to init, so nobody has to
    // reap it and it survives the editor.
    const pid_t intermediate = fork();
    if (intermediate == -1) {
        const int code = errno;
        execError.closeBoth();
        return failure(errnoMessage("Could not start process", code));
    }
    if (intermediate == 0) {
        setsid();
        const pid_t launched = fork();
        if (launched == 0) {
            execChild(*built.arguments, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), -1,
                      execError.write);
        }
        if (launched == -1) {
            const int code = errno;
            [[maybe_unused]] const ssize_t written = ::write(execError.write, &code, sizeof(code));
        }
        _exit(0);
    }
    execError.closeWrite();
    int status = 0;
    waitpid(intermediate, &status, 0);
    const std::optional<int> execFailure = readExecError(execError.read);
    execError.closeRead();
    if (execFailure) {
        return failure(errnoMessage("Could not start process", *execFailure));
    }
    return {.success = true};
}

#endif

ChildProcess::ChildProcess() = default;

ChildProcess::~ChildProcess() {
    finish();
}

ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        finish();
        m_state = std::move(other.m_state);
    }
    return *this;
}

bool ChildProcess::running() const {
    return m_state != nullptr && !m_state->finished;
}

std::optional<int> ChildProcess::exitCode() const {
    if (m_state == nullptr || !m_state->finished) {
        return std::nullopt;
    }
    const std::lock_guard lock(m_state->mutex);
    return m_state->exitCode;
}

bool ChildProcess::cancelled() const {
    return m_state != nullptr && m_state->cancelled;
}

std::vector<std::string> ChildProcess::takeLines() {
    if (m_state == nullptr) {
        return {};
    }
    const std::lock_guard lock(m_state->mutex);
    return std::exchange(m_state->lines, {});
}

void ChildProcess::cancel() {
    if (!running()) {
        return;
    }
    m_state->cancelled = true;
    m_state->terminate();
}

void ChildProcess::wait() {
    if (m_state != nullptr && m_state->reader.joinable()) {
        m_state->reader.join();
    }
}

void ChildProcess::finish() {
    if (m_state == nullptr) {
        return;
    }
    cancel();
    wait();
    m_state->release();
    m_state.reset();
}

uint32_t currentProcessId() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}
