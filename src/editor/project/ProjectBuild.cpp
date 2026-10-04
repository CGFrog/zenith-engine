#include "ProjectBuild.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "engine/platform/FileWatcher.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr const char* buildSource = "Build";

// A save burst settles before the build starts, so an IDE writing several
// files at once starts one build.
constexpr std::chrono::milliseconds buildOnSaveDebounce{300};

EditorDiagnosticSeverity editorSeverity(CompilerDiagnosticSeverity severity) {
    switch (severity) {
    case CompilerDiagnosticSeverity::Error:
        return EditorDiagnosticSeverity::Error;
    case CompilerDiagnosticSeverity::Warning:
        return EditorDiagnosticSeverity::Warning;
    case CompilerDiagnosticSeverity::Note:
        return EditorDiagnosticSeverity::Info;
    }
    return EditorDiagnosticSeverity::Info;
}

std::string describe(const CompilerDiagnostic& diagnostic) {
    return diagnostic.code.empty() ? diagnostic.message : diagnostic.code + ": " + diagnostic.message;
}

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return text;
}
} // namespace

bool buildOnSaveWatches(const std::filesystem::path& path) {
    if (lowercase(path.filename().string()) == "cmakelists.txt") {
        return true;
    }
    constexpr std::array<std::string_view, 8> extensions{".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl"};
    const std::string extension = lowercase(path.extension().string());
    return std::ranges::find(extensions, extension) != extensions.end();
}

ProjectBuild::ProjectBuild(const EditorToolSettings& tools, EditorDiagnostics& diagnostics)
    : m_tools(tools)
    , m_diagnostics(diagnostics) {}

ProjectBuild::~ProjectBuild() = default;

ChildProcessResult ProjectBuild::start() {
    m_queued = false;
    const ChildProcessResult started = m_runner.start(buildRequest());
    std::vector<EditorDiagnostic> replacements;
    if (!started) {
        replacements.push_back({.severity = EditorDiagnosticSeverity::Error,
                                .message = "Could not start the build: " + started.error,
                                .action = "Check build.command and build.directory in the Build panel."});
    }
    m_diagnostics.replaceSource(buildSource, std::move(replacements));
    ++m_startCount;
    return started;
}

void ProjectBuild::request() {
    if (m_runner.running()) {
        m_queued = true;
        return;
    }
    start();
}

void ProjectBuild::cancel() {
    m_queued = false;
    m_runner.cancel();
}

void ProjectBuild::poll() {
    if (m_runner.poll().finished) {
        publishDiagnostics();
    }
    watchSources();
    if (m_queued && !m_runner.running()) {
        start();
    }
}

const BuildRunner& ProjectBuild::runner() const {
    return m_runner;
}

bool ProjectBuild::running() const {
    return m_runner.running();
}

bool ProjectBuild::queued() const {
    return m_queued;
}

uint64_t ProjectBuild::startCount() const {
    return m_startCount;
}

void ProjectBuild::setProjectBuild(std::optional<BuildRequest> projectBuild) {
    m_projectBuild = std::move(projectBuild);
}

bool ProjectBuild::usesProjectBuild() const {
    const EditorToolSettings defaults;
    return m_projectBuild && m_tools.buildCommand == defaults.buildCommand &&
           m_tools.buildDirectory == defaults.buildDirectory;
}

BuildRequest ProjectBuild::buildRequest() const {
    if (usesProjectBuild()) {
        return *m_projectBuild;
    }
    return {.command = m_tools.buildCommand, .workingDirectory = pathFromUtf8Text(m_tools.buildDirectory)};
}

std::filesystem::path ProjectBuild::watchedSourceDirectory() const {
    if (!usesProjectBuild() || m_projectBuild->workingDirectory.empty()) {
        return {};
    }
    return m_projectBuild->workingDirectory / "src";
}

void ProjectBuild::publishDiagnostics() const {
    std::vector<EditorDiagnostic> replacements;
    bool reportedError = false;
    for (const CompilerDiagnostic& diagnostic : m_runner.diagnostics()) {
        reportedError = reportedError || diagnostic.severity == CompilerDiagnosticSeverity::Error;
        replacements.push_back({.severity = editorSeverity(diagnostic.severity),
                                .message = describe(diagnostic),
                                .location = diagnostic.location});
    }
    if (m_runner.state() == BuildState::Failed && !reportedError) {
        const int code = m_runner.exitCode().value_or(-1);
        replacements.push_back({.severity = EditorDiagnosticSeverity::Error,
                                .message = "Build failed with exit code " + std::to_string(code),
                                .action = "Review the Build panel output."});
    } else if (m_runner.state() == BuildState::Cancelled) {
        replacements.push_back({.severity = EditorDiagnosticSeverity::Info, .message = "Build cancelled"});
    }
    m_diagnostics.replaceSource(buildSource, std::move(replacements));
}

void ProjectBuild::watchSources() {
    const std::filesystem::path directory = m_tools.buildOnSave ? watchedSourceDirectory() : std::filesystem::path{};
    if (directory != m_watchedDirectory) {
        m_watcher.reset();
        m_watchedDirectory = directory;
        if (!directory.empty()) {
            const FileWatcherResult created =
                FileWatcher::create(directory, {.debounce = buildOnSaveDebounce}, m_watcher);
            if (!created && created.error != m_watcherError) {
                m_diagnostics.add(EditorDiagnosticSeverity::Warning, buildSource,
                                  "Build on save cannot watch " + utf8PathText(directory) + ": " + created.error);
            }
            m_watcherError = created ? std::string{} : created.error;
        }
    }
    if (!m_watcher) {
        return;
    }
    const std::vector<FileWatchEvent> events = m_watcher->poll();
    const bool sourceChanged = std::ranges::any_of(events, [](const FileWatchEvent& event) {
        return !event.isDirectory &&
               (event.kind == FileWatchEventKind::RescanRequired || buildOnSaveWatches(pathFromUtf8Text(event.path)));
    });
    if (sourceChanged) {
        request();
    }
}
