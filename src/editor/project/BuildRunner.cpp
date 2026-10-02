#include "BuildRunner.hpp"

#include <system_error>
#include <utility>

ChildProcessResult BuildRunner::start(const BuildRequest& request) {
    if (running()) {
        return {.error = "A build is already running"};
    }
    if (request.command.find_first_not_of(" \t") == std::string::npos) {
        return {.error = "No build command is configured"};
    }
    std::error_code error;
    if (!request.workingDirectory.empty() && !std::filesystem::is_directory(request.workingDirectory, error)) {
        return {.error = "Build directory does not exist: " + request.workingDirectory.string()};
    }
    const ChildProcessResult started = m_process.start(
        {.command = ChildProcessCommand::shell(request.command), .workingDirectory = request.workingDirectory});
    if (!started) {
        return started;
    }
    m_parser.clear();
    m_output.clear();
    m_command = request.command;
    m_workingDirectory = request.workingDirectory.empty() ? std::filesystem::current_path(error)
                                                          : std::filesystem::absolute(request.workingDirectory, error);
    m_state = BuildState::Running;
    m_exitCode.reset();
    return started;
}

BuildPollResult BuildRunner::poll() {
    if (m_state != BuildState::Running) {
        return {};
    }
    // Check for exit before taking lines: the process publishes all output
    // before it reports finishing, so nothing is left behind.
    const bool stillRunning = m_process.running();
    std::vector<std::string> lines = m_process.takeLines();
    BuildPollResult result{.newLines = lines.size()};
    for (std::string& line : lines) {
        appendLine(std::move(line));
    }
    if (!stillRunning) {
        m_exitCode = m_process.exitCode();
        if (m_process.cancelled()) {
            m_state = BuildState::Cancelled;
        } else {
            m_state = m_exitCode == 0 ? BuildState::Succeeded : BuildState::Failed;
        }
        result.finished = true;
    }
    return result;
}

void BuildRunner::cancel() {
    m_process.cancel();
}

BuildState BuildRunner::state() const {
    return m_state;
}

bool BuildRunner::running() const {
    return m_state == BuildState::Running;
}

std::optional<int> BuildRunner::exitCode() const {
    return m_exitCode;
}

const std::deque<BuildOutputLine>& BuildRunner::output() const {
    return m_output;
}

std::vector<CompilerDiagnostic> BuildRunner::diagnostics() const {
    std::vector<CompilerDiagnostic> diagnostics = m_parser.diagnostics();
    for (CompilerDiagnostic& diagnostic : diagnostics) {
        if (diagnostic.location) {
            diagnostic.location = resolve(std::move(*diagnostic.location));
        }
    }
    return diagnostics;
}

const std::string& BuildRunner::command() const {
    return m_command;
}

void BuildRunner::appendLine(std::string text) {
    BuildOutputLine line;
    if (std::optional<CompilerDiagnostic> diagnostic = m_parser.addLine(text)) {
        line.severity = diagnostic->severity;
        if (diagnostic->location) {
            line.location = resolve(std::move(*diagnostic->location));
        }
    }
    line.text = std::move(text);
    m_output.push_back(std::move(line));
    if (m_output.size() > maximumOutputLines) {
        m_output.pop_front();
    }
}

EditorSourceLocation BuildRunner::resolve(EditorSourceLocation location) const {
    if (location.file.is_relative() && !m_workingDirectory.empty()) {
        location.file = (m_workingDirectory / location.file).lexically_normal();
    }
    return location;
}
