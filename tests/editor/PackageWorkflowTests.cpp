#include "HeadlessImGui.hpp"

#include "EditorTestFixture.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/PackageOperators.hpp"
#include "editor/panels/BuildPanel.hpp"
#include "editor/project/PackageRunner.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool contains(const std::vector<std::string>& arguments, const std::string& value) {
    return std::ranges::find(arguments, value) != arguments.end();
}

// Plays back one scripted outcome per step instead of running processes.
class ScriptedStepRunner final : public PackageStepRunner {
  public:
    struct Outcome {
        BuildState state{BuildState::Succeeded};
        int exitCode{0};
        std::vector<BuildOutputLine> lines;
        bool startFails{false};
    };

    explicit ScriptedStepRunner(std::vector<Outcome> outcomes)
        : m_outcomes(std::move(outcomes)) {}

    ChildProcessResult start(const PackageStep& step, const std::filesystem::path& workingDirectory) override {
        started.push_back(step.label);
        lastWorkingDirectory = workingDirectory;
        const size_t index = started.size() - 1;
        if (index < m_outcomes.size() && m_outcomes[index].startFails) {
            return {.error = "no such program"};
        }
        m_polledOnce = false;
        return {.success = true};
    }

    PackageStepPoll poll() override {
        const size_t index = started.size() - 1;
        const Outcome outcome = index < m_outcomes.size() ? m_outcomes[index] : Outcome{};
        // The first poll streams output; the second reports the end.
        if (!m_polledOnce) {
            m_polledOnce = true;
            return {.lines = outcome.lines};
        }
        if (cancelRequested) {
            return {.finished = true, .state = BuildState::Cancelled};
        }
        return {.finished = true, .state = outcome.state, .exitCode = outcome.exitCode};
    }

    void cancel() override {
        cancelRequested = true;
    }

    std::vector<std::string> started;
    std::filesystem::path lastWorkingDirectory;
    bool cancelRequested{false};

  private:
    std::vector<Outcome> m_outcomes;
    bool m_polledOnce{false};
};

struct ScriptedRunner {
    ScriptedStepRunner* steps{nullptr};
    PackageRunner runner;

    explicit ScriptedRunner(std::vector<ScriptedStepRunner::Outcome> outcomes)
        : runner(makeSteps(std::move(outcomes))) {}

  private:
    std::unique_ptr<PackageStepRunner> makeSteps(std::vector<ScriptedStepRunner::Outcome> outcomes) {
        auto scripted = std::make_unique<ScriptedStepRunner>(std::move(outcomes));
        steps = scripted.get();
        return scripted;
    }
};

PackagePollResult pollToEnd(PackageRunner& runner) {
    PackagePollResult last;
    for (int attempt = 0; attempt < 100 && runner.running(); ++attempt) {
        last = runner.poll();
    }
    return last;
}

PackageRequest demoRequest() {
    return {.sourceDirectory = "C:/src/zenith",
            .buildDirectory = "C:/src/zenith/build-release",
            .projectFile = "C:/src/zenith/projects/my game.zenith-project",
            .platform = hostPackagePlatform(),
            .configuration = PackageConfiguration::Release};
}

void testPlanAssembly() {
    const PackagePlanResult planned = planPackage(demoRequest());
    expect(static_cast<bool>(planned), "a project on the host platform plans");
    const std::vector<PackageStep>& steps = planned.plan.steps;
    expect(steps.size() == 4, "configure, build, cook, and package");
    if (steps.size() == 4) {
        const std::vector<std::string>& configure = steps[0].arguments;
        expect(configure.front() == "cmake" && contains(configure, "-S") && contains(configure, "C:/src/zenith") &&
                   contains(configure, "C:/src/zenith/build-release"),
               "configure points CMake at the source and the release tree");
        expect(contains(configure, "-DZENITH_PACKAGE_PROJECT=C:/src/zenith/projects/my game.zenith-project"),
               "the project path stays one argument, spaces included");
        expect(contains(configure, "-DZENITH_COPY_PROJECT_MODULE=OFF") && contains(configure, "-DBUILD_TESTING=OFF") &&
                   contains(configure, "-DCMAKE_BUILD_TYPE=Release"),
               "the release tree never copies its module beside the project");
        expect(steps[1].arguments == std::vector<std::string>{"cmake", "--build", "C:/src/zenith/build-release",
                                                              "--config", "Release", "--parallel"},
               "the build step builds the configuration");
        expect(contains(steps[2].arguments, "zenith_cooked_textures"), "the cook step builds the cooked textures");
        expect(contains(steps[3].arguments, "package") && steps[3].label == "Package", "the last step packages");
    }
    expect(planned.plan.buildDirectory == std::filesystem::path("C:/src/zenith/build-release"),
           "steps run in the release tree");

    PackageRequest configured = demoRequest();
    configured.configure = false;
    configured.configuration = PackageConfiguration::RelWithDebInfo;
    const PackagePlanResult withoutConfigure = planPackage(configured);
    expect(withoutConfigure.plan.steps.size() == 3 &&
               contains(withoutConfigure.plan.steps[0].arguments, "RelWithDebInfo"),
           "a configured tree skips configuring and builds the chosen configuration");

    PackageRequest noProject = demoRequest();
    noProject.projectFile.clear();
    expect(planPackage(noProject).error.find("No project is open") != std::string::npos, "no project fails");
    PackageRequest scene = demoRequest();
    scene.projectFile = "C:/src/zenith/projects/scenes/demo.zenith-scene";
    expect(!planPackage(scene), "only .zenith-project files are packaged");
    PackageRequest sameTree = demoRequest();
    sameTree.buildDirectory = sameTree.sourceDirectory;
    expect(!planPackage(sameTree), "the release tree cannot be the source tree");

    for (const PackagePlatform other : {PackagePlatform::Windows, PackagePlatform::Linux, PackagePlatform::MacOS}) {
        if (other == hostPackagePlatform()) {
            continue;
        }
        PackageRequest cross = demoRequest();
        cross.platform = other;
        const PackagePlanResult refused = planPackage(cross);
        expect(!refused && refused.error.find(std::string(packagePlatformLabel(other))) != std::string::npos,
               "other platforms are refused with the reason");
        expect(!packagePlatformSupport(other).available && !packagePlatformSupport(other).reason.empty(),
               "other platforms are shown disabled with a reason");
    }
    expect(packagePlatformSupport(hostPackagePlatform()).available, "the host platform packages");
}

void testConfigureDetection() {
    const std::filesystem::path project = "C:/src/zenith/projects/demo.zenith-project";
    const std::string multiConfig =
        "# cache\r\nZENITH_PACKAGE_PROJECT:FILEPATH=C:/src/zenith/projects/demo.zenith-project\r\n"
        "ZENITH_COPY_PROJECT_MODULE:BOOL=OFF\r\nCMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\r\n";
    expect(!packageNeedsConfigure(multiConfig, project, PackageConfiguration::Release),
           "a release tree configured for the project is reused");
    expect(packageNeedsConfigure("", project, PackageConfiguration::Release), "a tree without a cache is configured");
    expect(packageNeedsConfigure(multiConfig, "C:/src/zenith/projects/showcase.zenith-project",
                                 PackageConfiguration::Release),
           "a tree configured for another project is configured again");
    std::string copying = multiConfig;
    copying.replace(copying.find("BOOL=OFF"), 8, "BOOL=ON");
    expect(packageNeedsConfigure(copying, project, PackageConfiguration::Release),
           "a tree that copies its module beside the project is configured again");
    const std::string singleConfig = "ZENITH_PACKAGE_PROJECT:FILEPATH=C:/src/zenith/projects/demo.zenith-project\n"
                                     "ZENITH_COPY_PROJECT_MODULE:BOOL=OFF\nCMAKE_BUILD_TYPE:STRING=Release\n";
    expect(!packageNeedsConfigure(singleConfig, project, PackageConfiguration::Release) &&
               packageNeedsConfigure(singleConfig, project, PackageConfiguration::RelWithDebInfo),
           "a single-configuration tree is configured again for another configuration");
}

void testRunnerSequence() {
    PackagePlan plan = planPackage(demoRequest()).plan;
    plan.buildDirectory = std::filesystem::temp_directory_path() / "zenith_package_workflow_tests" / "release";

    ScriptedRunner succeeding({{.lines = {{.text = "configured"}}}, {.lines = {{.text = "built"}}}, {}, {}});
    expect(static_cast<bool>(succeeding.runner.start(plan)), "a plan starts");
    expect(succeeding.runner.running() && succeeding.steps->started.size() == 1, "steps start one at a time");
    expect(!succeeding.runner.start(plan), "a second package cannot start while one runs");
    const PackagePollResult finished = pollToEnd(succeeding.runner);
    expect(finished.finished && succeeding.runner.state() == PackageState::Succeeded, "every step succeeding packages");
    expect(succeeding.steps->started ==
               std::vector<std::string>{"Configure Release", "Build Release", "Cook textures", "Package"},
           "the steps run in order");
    expect(succeeding.steps->lastWorkingDirectory == plan.buildDirectory, "steps run in the release tree");
    const auto& output = succeeding.runner.output();
    expect(std::ranges::any_of(output, [](const BuildOutputLine& line) { return line.text == "built"; }) &&
               output.front().text.find("[1/4] Configure Release") != std::string::npos,
           "the output keeps every step's lines under a header");
    const std::vector<EditorDiagnostic> success =
        packageDiagnostics(succeeding.runner, std::filesystem::path("C:/out/Zenith.zip"));
    expect(success.size() == 1 && success[0].severity == EditorDiagnosticSeverity::Info &&
               success[0].message.find("Zenith.zip") != std::string::npos,
           "a finished package reports its archive");

    ScriptedRunner failing({{},
                            {.state = BuildState::Failed,
                             .exitCode = 2,
                             .lines = {{.text = "a.cpp(1): error C1: broken",
                                        .severity = CompilerDiagnosticSeverity::Error,
                                        .location = EditorSourceLocation{"a.cpp", 1, 0}}}}});
    failing.runner.start(plan);
    pollToEnd(failing.runner);
    expect(failing.runner.state() == PackageState::Failed && failing.runner.currentStep() == 1 &&
               failing.runner.exitCode() == 2 && failing.steps->started.size() == 2,
           "a failing step stops the package at that step");
    expect(failing.runner.failure().find("Build Release") != std::string::npos, "the failure names its step");
    const std::vector<EditorDiagnostic> failure = packageDiagnostics(failing.runner, std::nullopt);
    expect(failure.size() == 2 && failure[0].location && failure[1].severity == EditorDiagnosticSeverity::Error &&
               !failure[1].action.empty(),
           "a failed package publishes its errors and a summary");

    ScriptedRunner cancelled({{}, {}, {}, {}});
    cancelled.runner.start(plan);
    cancelled.runner.poll();
    cancelled.runner.cancel();
    pollToEnd(cancelled.runner);
    expect(cancelled.runner.state() == PackageState::Cancelled && cancelled.steps->started.size() == 1,
           "cancelling stops the package without starting more steps");

    ScriptedRunner unstartable({{}, {.startFails = true}});
    unstartable.runner.start(plan);
    pollToEnd(unstartable.runner);
    expect(unstartable.runner.state() == PackageState::Failed &&
               unstartable.runner.failure().find("no such program") != std::string::npos,
           "a step that cannot start fails the package");

    ScriptedRunner empty({});
    expect(!empty.runner.start({}), "an empty plan does not start");
}

struct OperatorFixture {
    Scene scene;
    AssetManager assets;
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context{.editing = SceneEditing(scene, assets, document, history, diagnostics),
                                .selection = selection};
        return runner.invokeWithJson(id, context, properties);
    }
};

void testBuildPackageOperator() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_package_workflow_tests";
    const std::filesystem::path source = root / "engine source";
    std::filesystem::create_directories(source / "projects");
    std::ofstream(source / "CMakeLists.txt") << "project(Fake)\n";
    const std::filesystem::path project = source / "projects" / "game.zenith-project";
    std::ofstream(project) << "{}\n";

    EditorToolSettings tools;
    tools.buildDirectory = source.string();
    tools.packageDirectory = "out/release";
    const BuildPackageProperties properties;
    const PackageRequest request = packageRequestFor(tools, "projects/game.zenith-project", properties, source);
    expect(request.sourceDirectory == source.lexically_normal() &&
               request.buildDirectory == (source / "out" / "release").lexically_normal() &&
               request.projectFile == project.lexically_normal(),
           "the request resolves the build directory, package directory, and project");

    ScriptedRunner scripted({{}, {}, {}, {}});
    OperatorFixture fixture;
    expect(
        registerPackageOperators(fixture.registry, {.runner = scripted.runner, .tools = tools, .projectFile = project})
            .success,
        "file.build_package registers");
    expect(static_cast<bool>(fixture.invoke("file.build_package")), "file.build_package starts headless");
    expect(scripted.runner.running() && scripted.runner.plan().steps.size() == 4 &&
               scripted.runner.plan().projectFile == project.lexically_normal(),
           "an unconfigured release tree is configured first, for the open project");
    expect(!fixture.invoke("file.build_package"), "a second package does not start while one runs");

    // The Build panel follows the package and draws its output.
    EditorTestFixture panelFixture;
    ProjectBuild build(tools, panelFixture.diagnostics);
    BuildPanel panel(tools, build, &scripted.runner);
    {
        HeadlessImGuiContext imgui;
        {
            HeadlessImGuiFrame frame("Build");
            EditorContext context = panelFixture.context();
            panel.drawContents(context);
        }
        expect(panel.view() == BuildPanel::View::Package, "the Build panel shows a package when it starts");
        pollToEnd(scripted.runner);
        HeadlessImGuiFrame frame("Build");
        EditorContext context = panelFixture.context();
        panel.drawContents(context);
    }
    expect(scripted.runner.state() == PackageState::Succeeded, "the scripted package finishes");

    const std::string otherPlatform = hostPackagePlatform() == PackagePlatform::Linux ? "windows" : "linux";
    ScriptedRunner idle({});
    OperatorFixture crossFixture;
    registerPackageOperators(crossFixture.registry, {.runner = idle.runner, .tools = tools, .projectFile = project});
    const OperatorResult cross =
        crossFixture.invoke("file.build_package", R"({"platform":")" + otherPlatform + R"("})");
    expect(cross.status == OperatorStatus::Failed && cross.message.find("computer") != std::string::npos,
           "another platform fails with the reason");

    OperatorFixture noProjectFixture;
    registerPackageOperators(noProjectFixture.registry, {.runner = idle.runner, .tools = tools, .projectFile = {}});
    const OperatorResult noProject = noProjectFixture.invoke("file.build_package");
    expect(noProject.status == OperatorStatus::Failed && noProject.message.find("No project") != std::string::npos,
           "packaging without a project explains why");

    EditorToolSettings elsewhere = tools;
    elsewhere.buildDirectory = (root / "not a checkout").string();
    std::filesystem::create_directories(root / "not a checkout");
    OperatorFixture noSourceFixture;
    registerPackageOperators(noSourceFixture.registry,
                             {.runner = idle.runner, .tools = elsewhere, .projectFile = project});
    const OperatorResult noSource = noSourceFixture.invoke("file.build_package");
    expect(noSource.status == OperatorStatus::Failed && noSource.message.find("CMakeLists.txt") != std::string::npos,
           "a build directory without the engine checkout explains why");
    expect(idle.runner.state() == PackageState::Idle, "refused packages never start");

    std::ofstream(root / "Zenith-old.zip") << "old";
    std::ofstream(root / "notes.txt") << "notes";
    expect(newestPackageArchive(root) == root / "Zenith-old.zip", "the archive is found among the build outputs");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
} // namespace

int main() {
    testPlanAssembly();
    testConfigureDetection();
    testRunnerSequence();
    testBuildPackageOperator();
    if (failures != 0) {
        std::cerr << failures << " package workflow test(s) failed\n";
        return 1;
    }
    std::cout << "Package workflow tests passed\n";
    return 0;
}
