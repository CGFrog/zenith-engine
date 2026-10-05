#include "EditorTestFixture.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/operators/BuildOperators.hpp"
#include "editor/panels/BuildPanel.hpp"
#include "editor/panels/DiagnosticsPanel.hpp"
#include "editor/project/BuildRunner.hpp"
#include "editor/project/CompilerOutputParser.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/settings/EditorSettings.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// A headless ImGui frame: no renderer, just enough state for widgets to run.
class HeadlessFrame {
  public:
    HeadlessFrame() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        ImGui::Begin("Build test");
    }

    ~HeadlessFrame() {
        ImGui::End();
        ImGui::Render();
        ImGui::DestroyContext();
    }

    HeadlessFrame(const HeadlessFrame&) = delete;
    HeadlessFrame& operator=(const HeadlessFrame&) = delete;
};

bool located(const CompilerDiagnostic& diagnostic, const std::filesystem::path& file, uint32_t line, uint32_t column) {
    return diagnostic.location && diagnostic.location->file == file && diagnostic.location->line == line &&
           diagnostic.location->column == column;
}

void testMsvcParsing() {
    const std::optional<CompilerDiagnostic> error =
        parseCompilerOutputLine(R"(  3>C:\src\a b\main.cpp(12,5): error C2065: 'x': undeclared identifier )"
                                R"([C:\build\zenith.vcxproj])");
    expect(error && error->severity == CompilerDiagnosticSeverity::Error &&
               located(*error, R"(C:\src\a b\main.cpp)", 12, 5) && error->code == "C2065" &&
               error->message == "'x': undeclared identifier",
           "MSVC errors should parse with location, code, and without the MSBuild suffix");

    const std::optional<CompilerDiagnostic> warning =
        parseCompilerOutputLine(R"(C:\Program Files (x86)\sdk\x.h(7): warning C4100: 'p': unreferenced parameter)");
    expect(warning && warning->severity == CompilerDiagnosticSeverity::Warning &&
               located(*warning, R"(C:\Program Files (x86)\sdk\x.h)", 7, 0) && warning->code == "C4100",
           "MSVC warnings without a column should parse through parenthesised directories");

    const std::optional<CompilerDiagnostic> note =
        parseCompilerOutputLine(R"(C:\src\main.cpp(3): note: see declaration of 'y')");
    expect(note && note->severity == CompilerDiagnosticSeverity::Note && note->code.empty(), "MSVC notes should parse");

    const std::optional<CompilerDiagnostic> linker =
        parseCompilerOutputLine("LINK : fatal error LNK1104: cannot open file 'zenith.exe'");
    expect(linker && linker->severity == CompilerDiagnosticSeverity::Error && !linker->location &&
               linker->code == "LNK1104" && linker->message == "LINK: cannot open file 'zenith.exe'",
           "linker errors without a location should parse");

    const std::optional<CompilerDiagnostic> unresolved =
        parseCompilerOutputLine("main.obj : error LNK2019: unresolved external symbol foo referenced in function main");
    expect(unresolved && unresolved->code == "LNK2019" && !unresolved->location,
           "object-file linker errors should parse without a location");
}

void testGnuParsing() {
    const std::optional<CompilerDiagnostic> error =
        parseCompilerOutputLine("/home/me/src/main.cpp:12:5: error: 'x' was not declared in this scope");
    expect(error && error->severity == CompilerDiagnosticSeverity::Error &&
               located(*error, "/home/me/src/main.cpp", 12, 5) &&
               error->message == "'x' was not declared in this scope",
           "GCC errors should parse with line and column");

    const std::optional<CompilerDiagnostic> windowsPath =
        parseCompilerOutputLine("C:/src/main.cpp:4:2: warning: unused variable 'y' [-Wunused-variable]");
    expect(windowsPath && windowsPath->severity == CompilerDiagnosticSeverity::Warning &&
               located(*windowsPath, "C:/src/main.cpp", 4, 2),
           "Clang diagnostics with drive letters should parse");

    const std::optional<CompilerDiagnostic> noColumn = parseCompilerOutputLine("src/a.cpp:9: note: candidate");
    expect(noColumn && noColumn->severity == CompilerDiagnosticSeverity::Note && located(*noColumn, "src/a.cpp", 9, 0),
           "GCC notes without a column should parse");

    const std::optional<CompilerDiagnostic> fatal =
        parseCompilerOutputLine("main.cpp:1:10: fatal error: missing.hpp: No such file or directory");
    expect(fatal && fatal->severity == CompilerDiagnosticSeverity::Error &&
               fatal->message == "missing.hpp: No such "
                                 "file or directory",
           "fatal errors should parse as errors");

    expect(parseCompilerOutputLine("collect2: error: ld returned 1 exit status").has_value(),
           "GNU tool errors should parse");
    const std::optional<CompilerDiagnostic> undefinedReference =
        parseCompilerOutputLine("/usr/bin/ld: main.o: in function `main': main.cpp:(.text+0x5): undefined "
                                "reference to `foo()'");
    expect(undefinedReference && undefinedReference->severity == CompilerDiagnosticSeverity::Error &&
               !undefinedReference->location,
           "undefined references should parse as errors");

    const std::optional<CompilerDiagnostic> cmake =
        parseCompilerOutputLine("CMake Error at CMakeLists.txt:42 (add_executable):");
    expect(cmake && located(*cmake, "CMakeLists.txt", 42, 0) && cmake->message == "CMake Error in add_executable",
           "CMake errors should parse with their line");
}

void testIgnoredAndDeduplicated() {
    for (const char* line : {"", "[3/10] Building CXX object main.cpp.obj", "Build succeeded.", "    0 Warning(s)",
                             "    0 Error(s)", "In file included from a.hpp:3:", "1 error generated."}) {
        expect(!parseCompilerOutputLine(line).has_value(), "ordinary build output should not be a diagnostic");
    }
    const std::string longLine(100000, 'x');
    expect(!parseCompilerOutputLine(longLine + " error").has_value(), "very long lines should be handled");

    const std::vector<std::string> output{
        R"(1>C:\src\a.cpp(1,1): error C1: broken [C:\b\p.vcxproj])",
        "unrelated",
        R"(C:\src\a.cpp(1,1): error C1: broken [C:\b\p.vcxproj])",
        R"(C:\src\a.cpp(2,1): error C1: broken)",
    };
    const std::vector<CompilerDiagnostic> diagnostics = parseCompilerOutput(output);
    expect(diagnostics.size() == 2, "identical diagnostics (such as MSBuild's summary) should be deduplicated");
}

void testExternalEditorExpansion() {
    const EditorSourceLocation location{"C:/my src/a&b.cpp", 12, 0};
    const ExternalEditorCommandResult code =
        expandExternalEditorCommand(R"(code -g "{file}:{line}:{column}")", location);
    expect(code && code.arguments == std::vector<std::string>{"code", "-g", "C:/my src/a&b.cpp:12:1"},
           "placeholders should expand inside one argument with an unknown column as 1");

    const ExternalEditorCommandResult spaced =
        expandExternalEditorCommand(R"(  "C:/Program Files/IDE/ide.exe"  --line {line}   {file} {{x}} )", location);
    expect(spaced && spaced.arguments == std::vector<std::string>{"C:/Program Files/IDE/ide.exe", "--line", "12",
                                                                  "C:/my src/a&b.cpp", "{x}"},
           "quoted programs, extra whitespace, and escaped braces should tokenize");

    expect(!expandExternalEditorCommand("code {path}", location), "unknown placeholders should fail");
    expect(!expandExternalEditorCommand("code \"{file}", location), "unmatched quotes should fail");
    expect(!expandExternalEditorCommand("   ", location), "an empty command should fail");
    expect(!expandExternalEditorCommand("code {file", location), "unterminated placeholders should fail");
}

void testDiagnosticsReplacement() {
    EditorDiagnostics diagnostics;
    diagnostics.add(EditorDiagnosticSeverity::Warning, "Editor", "kept");
    diagnostics.replaceSource("Build", {{.message = "first"}, {.message = "second"}});
    diagnostics.replaceSource("Build", {{.message = "third", .location = EditorSourceLocation{"a.cpp", 1, 2}}});
    const std::vector<EditorDiagnostic>& entries = diagnostics.entries();
    expect(entries.size() == 2 && entries[0].message == "kept" && entries[1].message == "third" &&
               entries[1].source == "Build" && entries[1].location && entries[1].id != entries[0].id,
           "replacing a source should keep other sources and assign fresh ids");
}

BuildPollResult pollUntilFinished(BuildRunner& runner) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        const BuildPollResult result = runner.poll();
        if (result.finished) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return {};
}

std::string failingBuildCommand() {
#ifdef _WIN32
    return R"(echo C:\src\fake.cpp^(12,5^): error C2065: 'x': undeclared identifier&& )"
           R"(echo src/relative.cpp:3:7: warning: unused&& echo done&& exit /b 2)";
#else
    return "echo \"/src/fake.cpp:12:5: error: 'x' undeclared\"; "
           "echo 'src/relative.cpp:3:7: warning: unused'; echo done; exit 2";
#endif
}

void testBuildRunner(const std::filesystem::path& directory) {
    BuildRunner runner;
    expect(!runner.start({.command = "  "}).success, "an empty build command should fail");
    expect(!runner.start({.command = "echo x", .workingDirectory = directory / "missing"}).success,
           "a missing build directory should fail");
    expect(runner.state() == BuildState::Idle, "a failed start should leave the runner idle");

    expect(runner.start({.command = failingBuildCommand(), .workingDirectory = directory}).success,
           "the build should start");
    expect(runner.running(), "a started build should report running");
    expect(pollUntilFinished(runner).finished, "the build should finish");
    expect(runner.state() == BuildState::Failed && runner.exitCode() == 2,
           "a non-zero exit should fail the build with its exit code");

    const std::vector<CompilerDiagnostic> diagnostics = runner.diagnostics();
    expect(diagnostics.size() == 2, "the build output should yield two diagnostics");
    if (diagnostics.size() == 2) {
#ifdef _WIN32
        expect(located(diagnostics[0], R"(C:\src\fake.cpp)", 12, 5) && diagnostics[0].code == "C2065",
               "the MSVC error should be located");
#else
        expect(located(diagnostics[0], "/src/fake.cpp", 12, 5), "the GCC error should be located");
#endif
        const std::filesystem::path relative =
            (std::filesystem::absolute(directory) / "src/relative.cpp").lexically_normal();
        expect(located(diagnostics[1], relative, 3, 7) &&
                   diagnostics[1].severity == CompilerDiagnosticSeverity::Warning,
               "relative diagnostic paths should resolve against the build directory");
    }
    const std::deque<BuildOutputLine>& output = runner.output();
    expect(output.size() == 3 && output[0].severity == CompilerDiagnosticSeverity::Error && output[0].location &&
               !output[2].severity,
           "output lines should keep their text with highlighting and locations");
}

#ifdef _WIN32
constexpr const char* succeedingCommand = "echo all good&& exit /b 0";
constexpr const char* slowCommand = "ping -n 30 127.0.0.1 >nul";
#else
constexpr const char* succeedingCommand = "echo all good; exit 0";
constexpr const char* slowCommand = "sleep 30";
#endif

void waitForBuild(ProjectBuild& build, std::chrono::steady_clock::time_point deadline) {
    while ((build.running() || build.queued()) && std::chrono::steady_clock::now() < deadline) {
        build.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::vector<EditorDiagnostic> buildEntries(const EditorDiagnostics& diagnostics) {
    std::vector<EditorDiagnostic> entries;
    std::copy_if(diagnostics.entries().begin(), diagnostics.entries().end(), std::back_inserter(entries),
                 [](const EditorDiagnostic& entry) { return entry.source == "Build"; });
    return entries;
}

void testBuildPanel(const std::filesystem::path& directory) {
    EditorTestFixture fixture;
    EditorToolSettings tools;
    tools.buildCommand = failingBuildCommand();
    tools.buildDirectory = directory.string();
    // The opener is cmake itself, so the click path launches something real
    // without depending on an installed IDE.
    tools.ideCommand = std::string("\"") + ZENITH_TEST_CMAKE + "\" -E echo {file}:{line}";
    EditorDiagnostics& diagnostics = fixture.diagnostics;
    ProjectBuild build(tools, diagnostics);
    expect(static_cast<bool>(registerBuildOperators(fixture.operators, {.build = build})),
           "the build operators should register");
    BuildPanel panel(tools, build);
    DiagnosticsPanel diagnosticsPanel(tools);
    diagnostics.add(EditorDiagnosticSeverity::Info, "Editor", "unrelated");

    expect(!fixture.operators.find("build.cancel")->canRun(fixture.operatorContext()),
           "build.cancel should not run without a build");
    expect(static_cast<bool>(fixture.invoke("build.run")), "build.run should start the build headless");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    waitForBuild(build, deadline);
    const std::vector<EditorDiagnostic> failed = buildEntries(diagnostics);
    expect(failed.size() == 2 && failed[0].severity == EditorDiagnosticSeverity::Error && failed[0].location &&
               failed[0].location->line == 12 && failed[1].severity == EditorDiagnosticSeverity::Warning,
           "a finished build should publish its located diagnostics");

    {
        HeadlessFrame frame;
        EditorContext context = fixture.context();
        panel.drawContents(context);
        diagnosticsPanel.drawDiagnostics(diagnostics);
    }

    const size_t before = diagnostics.entries().size();
    openSourceLocation(tools.ideCommand, *failed[0].location, diagnostics);
    expect(diagnostics.entries().size() == before, "a valid IDE command should open without a diagnostic");
    openSourceLocation("ide {unknown}", *failed[0].location, diagnostics);
    expect(diagnostics.entries().size() == before + 1 && diagnostics.entries().back().source == "IDE",
           "an invalid IDE command should be reported as a diagnostic");

    tools.buildCommand = succeedingCommand;
    expect(static_cast<bool>(fixture.invoke("build.run")), "the second build should start");
    waitForBuild(build, deadline);
    expect(build.runner().state() == BuildState::Succeeded, "the second build should succeed");
    const std::vector<EditorDiagnostic> succeeded = buildEntries(diagnostics);
    expect(succeeded.size() == 1 && succeeded[0].severity == EditorDiagnosticSeverity::Info &&
               succeeded[0].message.starts_with("Built in ") && build.runner().duration().has_value(),
           "a clean build should replace the previous build's diagnostics with how long it took");
    expect(diagnostics.entries().front().message == "unrelated", "other diagnostics should be untouched");

    // A build requested while one runs is queued once, and cancelling drops
    // both.
    tools.buildCommand = slowCommand;
    const uint64_t startsBefore = build.startCount();
    fixture.invoke("build.run");
    fixture.invoke("build.run");
    fixture.invoke("build.run");
    expect(build.running() && build.queued() && build.startCount() == startsBefore + 1,
           "build.run during a build should queue one more build");
    expect(static_cast<bool>(fixture.invoke("build.cancel")), "build.cancel should stop the build");
    waitForBuild(build, std::chrono::steady_clock::now() + std::chrono::seconds(10));
    expect(!build.running() && !build.queued() && build.runner().state() == BuildState::Cancelled &&
               build.startCount() == startsBefore + 1,
           "a cancelled build should not start the queued one");
    const std::vector<EditorDiagnostic> cancelled = buildEntries(diagnostics);
    expect(cancelled.size() == 1 && cancelled[0].message == "Build cancelled", "a cancelled build is reported");

    tools.buildCommand = "";
    expect(!fixture.invoke("build.run"), "a build that cannot start should fail the operator");
    const std::vector<EditorDiagnostic> startFailure = buildEntries(diagnostics);
    expect(startFailure.size() == 1 && startFailure[0].severity == EditorDiagnosticSeverity::Error,
           "a build that cannot start should report one error");
    {
        HeadlessFrame frame;
        EditorContext context = fixture.context();
        panel.drawContents(context);
    }
}

void testBuildOnSave(const std::filesystem::path& directory) {
    expect(buildOnSaveWatches("Player.cpp") && buildOnSaveWatches("nested/Player.HPP") &&
               buildOnSaveWatches("CMakeLists.txt") && buildOnSaveWatches("a.inl"),
           "build on save should watch sources, headers, and CMake lists");
    expect(!buildOnSaveWatches("notes.md") && !buildOnSaveWatches("Player.cpp.tmp") &&
               !buildOnSaveWatches("build.ninja"),
           "build on save should ignore other files");

    const std::filesystem::path project = directory / "on save project";
    std::filesystem::create_directories(project / "src");
    EditorToolSettings tools;
    EditorDiagnostics diagnostics;
    ProjectBuild build(tools, diagnostics);
    build.setProjectBuild(BuildRequest{.command = succeedingCommand, .workingDirectory = project});
    expect(build.watchedSourceDirectory() == project / "src", "the project's src folder should be watched");

    // Off by default: a saved source starts nothing.
    build.poll();
    std::ofstream(project / "src" / "First.cpp") << "// first\n";
    const auto quiet = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < quiet) {
        build.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(build.startCount() == 0, "a save should not build while build on save is off");

    tools.buildOnSave = true;
    build.poll();
    std::ofstream(project / "src" / "Second.cpp") << "// second\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (build.startCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        build.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(build.startCount() == 1, "saving a source file should start a build");
    waitForBuild(build, deadline);
    expect(build.runner().state() == BuildState::Succeeded, "the build on save should succeed");

    // Editing the build command stops using the project's build, and with it
    // the watcher.
    tools.buildCommand = succeedingCommand;
    expect(build.watchedSourceDirectory().empty(), "a custom build command should not be watched");
}
} // namespace

int main() {
    testMsvcParsing();
    testGnuParsing();
    testIgnoredAndDeduplicated();
    testExternalEditorExpansion();
    testDiagnosticsReplacement();

    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_build_workflow_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    testBuildRunner(directory);
    testBuildPanel(directory);
    testBuildOnSave(directory);
    std::filesystem::remove_all(directory, error);

    if (failures != 0) {
        std::cerr << failures << " build workflow test(s) failed\n";
        return 1;
    }
    return 0;
}
