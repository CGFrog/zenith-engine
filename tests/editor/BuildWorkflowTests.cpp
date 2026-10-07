#include "EditorTestFixture.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/operators/BuildOperators.hpp"
#include "editor/panels/BuildPanel.hpp"
#include "editor/panels/DiagnosticsPanel.hpp"
#include "editor/project/BuildRunner.hpp"
#include "editor/project/CompilerOutputParser.hpp"
#include "editor/project/DeveloperEnvironment.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/settings/EditorSettings.hpp"

#include "support/Doctest.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

TEST_SUITE_BEGIN("BuildWorkflow");

namespace {

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

TEST_CASE("BuildWorkflow: msvc parsing") {
    const std::optional<CompilerDiagnostic> error =
        parseCompilerOutputLine(R"(  3>C:\src\a b\main.cpp(12,5): error C2065: 'x': undeclared identifier )"
                                R"([C:\build\zenith.vcxproj])");
    CHECK_MESSAGE((error && error->severity == CompilerDiagnosticSeverity::Error &&
                   located(*error, R"(C:\src\a b\main.cpp)", 12, 5) && error->code == "C2065" &&
                   error->message == "'x': undeclared identifier"),
                  "MSVC errors should parse with location, code, and without the MSBuild suffix");

    const std::optional<CompilerDiagnostic> warning =
        parseCompilerOutputLine(R"(C:\Program Files (x86)\sdk\x.h(7): warning C4100: 'p': unreferenced parameter)");
    CHECK_MESSAGE((warning && warning->severity == CompilerDiagnosticSeverity::Warning &&
                   located(*warning, R"(C:\Program Files (x86)\sdk\x.h)", 7, 0) && warning->code == "C4100"),
                  "MSVC warnings without a column should parse through parenthesised directories");

    const std::optional<CompilerDiagnostic> note =
        parseCompilerOutputLine(R"(C:\src\main.cpp(3): note: see declaration of 'y')");
    CHECK_MESSAGE((note && note->severity == CompilerDiagnosticSeverity::Note && note->code.empty()),
                  "MSVC notes should parse");

    const std::optional<CompilerDiagnostic> linker =
        parseCompilerOutputLine("LINK : fatal error LNK1104: cannot open file 'zenith.exe'");
    CHECK_MESSAGE((linker && linker->severity == CompilerDiagnosticSeverity::Error && !linker->location &&
                   linker->code == "LNK1104" && linker->message == "LINK: cannot open file 'zenith.exe'"),
                  "linker errors without a location should parse");

    const std::optional<CompilerDiagnostic> unresolved =
        parseCompilerOutputLine("main.obj : error LNK2019: unresolved external symbol foo referenced in function main");
    CHECK_MESSAGE((unresolved && unresolved->code == "LNK2019" && !unresolved->location),
                  "object-file linker errors should parse without a location");
}

TEST_CASE("BuildWorkflow: gnu parsing") {
    const std::optional<CompilerDiagnostic> error =
        parseCompilerOutputLine("/home/me/src/main.cpp:12:5: error: 'x' was not declared in this scope");
    CHECK_MESSAGE((error && error->severity == CompilerDiagnosticSeverity::Error &&
                   located(*error, "/home/me/src/main.cpp", 12, 5) &&
                   error->message == "'x' was not declared in this scope"),
                  "GCC errors should parse with line and column");

    const std::optional<CompilerDiagnostic> windowsPath =
        parseCompilerOutputLine("C:/src/main.cpp:4:2: warning: unused variable 'y' [-Wunused-variable]");
    CHECK_MESSAGE((windowsPath && windowsPath->severity == CompilerDiagnosticSeverity::Warning &&
                   located(*windowsPath, "C:/src/main.cpp", 4, 2)),
                  "Clang diagnostics with drive letters should parse");

    const std::optional<CompilerDiagnostic> noColumn = parseCompilerOutputLine("src/a.cpp:9: note: candidate");
    CHECK_MESSAGE(
        (noColumn && noColumn->severity == CompilerDiagnosticSeverity::Note && located(*noColumn, "src/a.cpp", 9, 0)),
        "GCC notes without a column should parse");

    const std::optional<CompilerDiagnostic> fatal =
        parseCompilerOutputLine("main.cpp:1:10: fatal error: missing.hpp: No such file or directory");
    CHECK_MESSAGE((fatal && fatal->severity == CompilerDiagnosticSeverity::Error &&
                   fatal->message == "missing.hpp: No such "
                                     "file or directory"),
                  "fatal errors should parse as errors");

    CHECK_MESSAGE(parseCompilerOutputLine("collect2: error: ld returned 1 exit status").has_value(),
                  "GNU tool errors should parse");
    const std::optional<CompilerDiagnostic> undefinedReference =
        parseCompilerOutputLine("/usr/bin/ld: main.o: in function `main': main.cpp:(.text+0x5): undefined "
                                "reference to `foo()'");
    CHECK_MESSAGE((undefinedReference && undefinedReference->severity == CompilerDiagnosticSeverity::Error &&
                   !undefinedReference->location),
                  "undefined references should parse as errors");

    const std::optional<CompilerDiagnostic> cmake =
        parseCompilerOutputLine("CMake Error at CMakeLists.txt:42 (add_executable):");
    CHECK_MESSAGE(
        (cmake && located(*cmake, "CMakeLists.txt", 42, 0) && cmake->message == "CMake Error in add_executable"),
        "CMake errors should parse with their line");
}

TEST_CASE("BuildWorkflow: ignored and deduplicated") {
    for (const char* line : {"", "[3/10] Building CXX object main.cpp.obj", "Build succeeded.", "    0 Warning(s)",
                             "    0 Error(s)", "In file included from a.hpp:3:", "1 error generated."}) {
        CHECK_MESSAGE(!parseCompilerOutputLine(line).has_value(), "ordinary build output should not be a diagnostic");
    }
    const std::string longLine(100000, 'x');
    CHECK_MESSAGE(!parseCompilerOutputLine(longLine + " error").has_value(), "very long lines should be handled");

    const std::vector<std::string> output{
        R"(1>C:\src\a.cpp(1,1): error C1: broken [C:\b\p.vcxproj])",
        "unrelated",
        R"(C:\src\a.cpp(1,1): error C1: broken [C:\b\p.vcxproj])",
        R"(C:\src\a.cpp(2,1): error C1: broken)",
    };
    const std::vector<CompilerDiagnostic> diagnostics = parseCompilerOutput(output);
    CHECK_MESSAGE(diagnostics.size() == 2, "identical diagnostics (such as MSBuild's summary) should be deduplicated");
}

TEST_CASE("BuildWorkflow: external editor expansion") {
    const EditorSourceLocation location{"C:/my src/a&b.cpp", 12, 0};
    const ExternalEditorCommandResult code =
        expandExternalEditorCommand(R"(code -g "{file}:{line}:{column}")", location);
    CHECK_MESSAGE((code && code.arguments == std::vector<std::string>{"code", "-g", "C:/my src/a&b.cpp:12:1"}),
                  "placeholders should expand inside one argument with an unknown column as 1");

    const ExternalEditorCommandResult spaced =
        expandExternalEditorCommand(R"(  "C:/Program Files/IDE/ide.exe"  --line {line}   {file} {{x}} )", location);
    CHECK_MESSAGE((spaced && spaced.arguments == std::vector<std::string>{"C:/Program Files/IDE/ide.exe", "--line",
                                                                          "12", "C:/my src/a&b.cpp", "{x}"}),
                  "quoted programs, extra whitespace, and escaped braces should tokenize");

    CHECK_MESSAGE(!expandExternalEditorCommand("code {path}", location), "unknown placeholders should fail");
    CHECK_MESSAGE(!expandExternalEditorCommand("code \"{file}", location), "unmatched quotes should fail");
    CHECK_MESSAGE(!expandExternalEditorCommand("   ", location), "an empty command should fail");
    CHECK_MESSAGE(!expandExternalEditorCommand("code {file", location), "unterminated placeholders should fail");
}

TEST_CASE("BuildWorkflow: diagnostics replacement") {
    EditorDiagnostics diagnostics;
    diagnostics.add(EditorDiagnosticSeverity::Warning, "Editor", "kept");
    diagnostics.replaceSource("Build", {{.message = "first"}, {.message = "second"}});
    diagnostics.replaceSource("Build", {{.message = "third", .location = EditorSourceLocation{"a.cpp", 1, 2}}});
    const std::vector<EditorDiagnostic>& entries = diagnostics.entries();
    CHECK_MESSAGE((entries.size() == 2 && entries[0].message == "kept" && entries[1].message == "third" &&
                   entries[1].source == "Build" && entries[1].location && entries[1].id != entries[0].id),
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

TEST_CASE("BuildWorkflow: build runner") {
    const testsupport::TempDirectory root("build-runner");
    const std::filesystem::path& directory = root.path();
    BuildRunner runner;
    CHECK_MESSAGE(!runner.start({.command = "  "}).success, "an empty build command should fail");
    CHECK_MESSAGE(!runner.start({.command = "echo x", .workingDirectory = directory / "missing"}).success,
                  "a missing build directory should fail");
    CHECK_MESSAGE(runner.state() == BuildState::Idle, "a failed start should leave the runner idle");

    CHECK_MESSAGE(runner.start({.command = failingBuildCommand(), .workingDirectory = directory}).success,
                  "the build should start");
    CHECK_MESSAGE(runner.running(), "a started build should report running");
    CHECK_MESSAGE(pollUntilFinished(runner).finished, "the build should finish");
    CHECK_MESSAGE((runner.state() == BuildState::Failed && runner.exitCode() == 2),
                  "a non-zero exit should fail the build with its exit code");

    const std::vector<CompilerDiagnostic> diagnostics = runner.diagnostics();
    CHECK_MESSAGE(diagnostics.size() == 2, "the build output should yield two diagnostics");
    if (diagnostics.size() == 2) {
#ifdef _WIN32
        CHECK_MESSAGE((located(diagnostics[0], R"(C:\src\fake.cpp)", 12, 5) && diagnostics[0].code == "C2065"),
                      "the MSVC error should be located");
#else
        CHECK_MESSAGE(located(diagnostics[0], "/src/fake.cpp", 12, 5), "the GCC error should be located");
#endif
        const std::filesystem::path relative =
            (std::filesystem::absolute(directory) / "src/relative.cpp").lexically_normal();
        CHECK_MESSAGE(
            (located(diagnostics[1], relative, 3, 7) && diagnostics[1].severity == CompilerDiagnosticSeverity::Warning),
            "relative diagnostic paths should resolve against the build directory");
    }
    const std::deque<BuildOutputLine>& output = runner.output();
    CHECK_MESSAGE((output.size() == 3 && output[0].severity == CompilerDiagnosticSeverity::Error &&
                   output[0].location && !output[2].severity),
                  "output lines should keep their text with highlighting and locations");
}

bool hasLine(const BuildRunner& runner, std::string_view text) {
    return std::ranges::any_of(runner.output(), [text](const BuildOutputLine& line) { return line.text == text; });
}

// A request's prepare step (configuring a build tree once) runs before the
// build while its marker is missing, joins the build's output, and stops the
// build when it fails; both steps get the request's environment.
TEST_CASE("BuildWorkflow: prepare step") {
    const testsupport::TempDirectory root("build-prepare-step");
    const std::filesystem::path& directory = root.path();
    const std::string cmake = ZENITH_TEST_CMAKE;
    const BuildRequest prepared{.command = "echo",
                                .arguments = {cmake, "-E", "echo", "built"},
                                .workingDirectory = directory,
                                .prepareArguments = {cmake, "-E", "echo", "prepared"},
                                .prepareUnlessExists = directory / "missing-marker"};
    BuildRunner runner;
    CHECK_MESSAGE((runner.start(prepared).success && pollUntilFinished(runner).finished),
                  "a prepared build should run");
    CHECK_MESSAGE((runner.state() == BuildState::Succeeded && hasLine(runner, "prepared") && hasLine(runner, "built")),
                  "the prepare step runs first while its marker is missing, and the build follows");

    BuildRequest skipped = prepared;
    skipped.prepareUnlessExists = directory;
    CHECK_MESSAGE((runner.start(skipped).success && pollUntilFinished(runner).finished), "a build should run again");
    CHECK_MESSAGE((runner.state() == BuildState::Succeeded && !hasLine(runner, "prepared") && hasLine(runner, "built")),
                  "the prepare step is skipped once its marker exists");

    BuildRequest failing = prepared;
    failing.prepareArguments = {cmake, "-E", "false"};
    CHECK_MESSAGE((runner.start(failing).success && pollUntilFinished(runner).finished),
                  "a failing prepare should finish");
    CHECK_MESSAGE((runner.state() == BuildState::Failed && !hasLine(runner, "built")),
                  "a failed prepare step fails the build without running it");

    const BuildRequest withEnvironment{.command = "environment",
                                       .arguments = {cmake, "-E", "environment"},
                                       .workingDirectory = directory,
                                       .environment = {{"ZENITH_BUILD_TEST", "from the request"}}};
    CHECK_MESSAGE((runner.start(withEnvironment).success && pollUntilFinished(runner).finished),
                  "a build with an environment should run");
    CHECK_MESSAGE(hasLine(runner, "ZENITH_BUILD_TEST=from the request"), "the build gets the request's environment");
}

TEST_CASE("BuildWorkflow: developer environment listing") {
    const std::vector<std::pair<std::string, std::string>> parsed =
        parseEnvironmentListing("Path=C:\\a;C:\\b\r\nINCLUDE=C:\\inc\r\n=C:=C:\\work\r\nnot a variable\r\nEMPTY=\r\n");
    CHECK_MESSAGE((parsed.size() == 3 && parsed[0] == std::pair<std::string, std::string>{"Path", "C:\\a;C:\\b"} &&
                   parsed[1].first == "INCLUDE" && parsed[2] == std::pair<std::string, std::string>{"EMPTY", ""}),
                  "a `set` listing parses into variables, skipping lines without a name");
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

TEST_CASE("BuildWorkflow: build panel") {
    const testsupport::TempDirectory root("build-panel");
    const std::filesystem::path& directory = root.path();
    EditorTestFixture fixture;
    EditorToolSettings tools;
    tools.buildCommand = failingBuildCommand();
    tools.buildDirectory = directory.string();
    // The opener is cmake itself, so the click path launches something real
    // without depending on an installed IDE.
    tools.ideCommand = std::string("\"") + ZENITH_TEST_CMAKE + "\" -E echo {file}:{line}";
    EditorDiagnostics& diagnostics = fixture.diagnostics;
    ProjectBuild build(tools, diagnostics);
    CHECK_MESSAGE(static_cast<bool>(registerBuildOperators(fixture.operators, {.build = build})),
                  "the build operators should register");
    BuildPanel panel(tools, build);
    DiagnosticsPanel diagnosticsPanel(tools);
    diagnostics.add(EditorDiagnosticSeverity::Info, "Editor", "unrelated");

    CHECK_MESSAGE(!fixture.operators.find("build.cancel")->canRun(fixture.operatorContext()),
                  "build.cancel should not run without a build");
    CHECK_MESSAGE(static_cast<bool>(fixture.invoke("build.run")), "build.run should start the build headless");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    waitForBuild(build, deadline);
    const std::vector<EditorDiagnostic> failed = buildEntries(diagnostics);
    CHECK_MESSAGE((failed.size() == 2 && failed[0].severity == EditorDiagnosticSeverity::Error && failed[0].location &&
                   failed[0].location->line == 12 && failed[1].severity == EditorDiagnosticSeverity::Warning),
                  "a finished build should publish its located diagnostics");

    {
        HeadlessFrame frame;
        EditorContext context = fixture.context();
        panel.drawContents(context);
        diagnosticsPanel.drawDiagnostics(diagnostics);
    }

    const size_t before = diagnostics.entries().size();
    openSourceLocation(tools.ideCommand, *failed[0].location, diagnostics);
    CHECK_MESSAGE(diagnostics.entries().size() == before, "a valid IDE command should open without a diagnostic");
    openSourceLocation("ide {unknown}", *failed[0].location, diagnostics);
    CHECK_MESSAGE((diagnostics.entries().size() == before + 1 && diagnostics.entries().back().source == "IDE"),
                  "an invalid IDE command should be reported as a diagnostic");

    tools.buildCommand = succeedingCommand;
    CHECK_MESSAGE(static_cast<bool>(fixture.invoke("build.run")), "the second build should start");
    waitForBuild(build, deadline);
    CHECK_MESSAGE(build.runner().state() == BuildState::Succeeded, "the second build should succeed");
    const std::vector<EditorDiagnostic> succeeded = buildEntries(diagnostics);
    CHECK_MESSAGE((succeeded.size() == 1 && succeeded[0].severity == EditorDiagnosticSeverity::Info &&
                   succeeded[0].message.starts_with("Built in ") && build.runner().duration().has_value()),
                  "a clean build should replace the previous build's diagnostics with how long it took");
    CHECK_MESSAGE(diagnostics.entries().front().message == "unrelated", "other diagnostics should be untouched");

    // A build requested while one runs is queued once, and cancelling drops
    // both.
    tools.buildCommand = slowCommand;
    const uint64_t startsBefore = build.startCount();
    fixture.invoke("build.run");
    fixture.invoke("build.run");
    fixture.invoke("build.run");
    CHECK_MESSAGE((build.running() && build.queued() && build.startCount() == startsBefore + 1),
                  "build.run during a build should queue one more build");
    CHECK_MESSAGE(static_cast<bool>(fixture.invoke("build.cancel")), "build.cancel should stop the build");
    waitForBuild(build, std::chrono::steady_clock::now() + std::chrono::seconds(10));
    CHECK_MESSAGE((!build.running() && !build.queued() && build.runner().state() == BuildState::Cancelled &&
                   build.startCount() == startsBefore + 1),
                  "a cancelled build should not start the queued one");
    const std::vector<EditorDiagnostic> cancelled = buildEntries(diagnostics);
    CHECK_MESSAGE((cancelled.size() == 1 && cancelled[0].message == "Build cancelled"),
                  "a cancelled build is reported");

    tools.buildCommand = "";
    CHECK_MESSAGE(!fixture.invoke("build.run"), "a build that cannot start should fail the operator");
    const std::vector<EditorDiagnostic> startFailure = buildEntries(diagnostics);
    CHECK_MESSAGE((startFailure.size() == 1 && startFailure[0].severity == EditorDiagnosticSeverity::Error),
                  "a build that cannot start should report one error");
    {
        HeadlessFrame frame;
        EditorContext context = fixture.context();
        panel.drawContents(context);
    }
}

TEST_CASE("BuildWorkflow: build on save") {
    const testsupport::TempDirectory root("build-on-save");
    const std::filesystem::path& directory = root.path();
    CHECK_MESSAGE((buildOnSaveWatches("Player.cpp") && buildOnSaveWatches("nested/Player.HPP") &&
                   buildOnSaveWatches("CMakeLists.txt") && buildOnSaveWatches("a.inl")),
                  "build on save should watch sources, headers, and CMake lists");
    CHECK_MESSAGE((!buildOnSaveWatches("notes.md") && !buildOnSaveWatches("Player.cpp.tmp") &&
                   !buildOnSaveWatches("build.ninja")),
                  "build on save should ignore other files");

    const std::filesystem::path project = directory / "on save project";
    std::filesystem::create_directories(project / "src");
    EditorToolSettings tools;
    EditorDiagnostics diagnostics;
    ProjectBuild build(tools, diagnostics);
    build.setProjectBuild(BuildRequest{.command = succeedingCommand, .workingDirectory = project});
    CHECK_MESSAGE(build.watchedSourceDirectory() == project / "src", "the project's src folder should be watched");

    // Off: a saved source starts nothing.
    tools.buildOnSave = false;
    build.poll();
    std::ofstream(project / "src" / "First.cpp") << "// first\n";
    const auto quiet = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < quiet) {
        build.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_MESSAGE(build.startCount() == 0, "a save should not build while build on save is off");

    tools.buildOnSave = true;
    build.poll();
    std::ofstream(project / "src" / "Second.cpp") << "// second\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (build.startCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        build.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_MESSAGE(build.startCount() == 1, "saving a source file should start a build");
    waitForBuild(build, deadline);
    CHECK_MESSAGE(build.runner().state() == BuildState::Succeeded, "the build on save should succeed");

    // Editing the build command stops using the project's build, and with it
    // the watcher.
    tools.buildCommand = succeedingCommand;
    CHECK_MESSAGE(build.watchedSourceDirectory().empty(), "a custom build command should not be watched");
}
} // namespace

TEST_SUITE_END();
