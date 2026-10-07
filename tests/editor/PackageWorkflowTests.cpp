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

#include "support/Doctest.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("PackageWorkflow");

namespace {

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

TEST_CASE("PackageWorkflow: plan assembly") {
    const PackagePlanResult planned = planPackage(demoRequest());
    CHECK_MESSAGE((static_cast<bool>(planned)), "a project on the host platform plans");
    const std::vector<PackageStep>& steps = planned.plan.steps;
    CHECK_MESSAGE(steps.size() == 4, "configure, build, cook, and package");
    if (steps.size() == 4) {
        const std::vector<std::string>& configure = steps[0].arguments;
        CHECK_MESSAGE((configure.front() == "cmake" && contains(configure, "-S") &&
                       contains(configure, "C:/src/zenith") && contains(configure, "C:/src/zenith/build-release")),
                      "configure points CMake at the source and the release tree");
        CHECK_MESSAGE(contains(configure, "-DZENITH_PACKAGE_PROJECT=C:/src/zenith/projects/my game.zenith-project"),
                      "the project path stays one argument, spaces included");
        CHECK_MESSAGE((contains(configure, "-DZENITH_COPY_PROJECT_MODULE=OFF") &&
                       contains(configure, "-DBUILD_TESTING=OFF") && contains(configure, "-DCMAKE_BUILD_TYPE=Release")),
                      "the release tree never copies its module beside the project");
        CHECK_MESSAGE((steps[1].arguments == std::vector<std::string>{"cmake", "--build", "C:/src/zenith/build-release",
                                                                      "--config", "Release", "--parallel"}),
                      "the build step builds the configuration");
        CHECK_MESSAGE(contains(steps[2].arguments, "zenith_cooked_textures"),
                      "the cook step builds the cooked textures");
        CHECK_MESSAGE((contains(steps[3].arguments, "package") && steps[3].label == "Package"),
                      "the last step packages");
    }
    CHECK_MESSAGE(planned.plan.buildDirectory == std::filesystem::path("C:/src/zenith/build-release"),
                  "steps run in the release tree");

    PackageRequest configured = demoRequest();
    configured.configure = false;
    configured.configuration = PackageConfiguration::RelWithDebInfo;
    const PackagePlanResult withoutConfigure = planPackage(configured);
    CHECK_MESSAGE((withoutConfigure.plan.steps.size() == 3 &&
                   contains(withoutConfigure.plan.steps[0].arguments, "RelWithDebInfo")),
                  "a configured tree skips configuring and builds the chosen configuration");

    PackageRequest noProject = demoRequest();
    noProject.projectFile.clear();
    CHECK_MESSAGE(planPackage(noProject).error.find("No project is open") != std::string::npos, "no project fails");
    PackageRequest scene = demoRequest();
    scene.projectFile = "C:/src/zenith/projects/scenes/demo.zenith-scene";
    CHECK_MESSAGE(!planPackage(scene), "only .zenith-project files are packaged");
    PackageRequest sameTree = demoRequest();
    sameTree.buildDirectory = sameTree.sourceDirectory;
    CHECK_MESSAGE(!planPackage(sameTree), "the release tree cannot be the source tree");

    for (const PackagePlatform other : {PackagePlatform::Windows, PackagePlatform::Linux, PackagePlatform::MacOS}) {
        if (other == hostPackagePlatform()) {
            continue;
        }
        PackageRequest cross = demoRequest();
        cross.platform = other;
        const PackagePlanResult refused = planPackage(cross);
        CHECK_MESSAGE((!refused && refused.error.find(std::string(packagePlatformLabel(other))) != std::string::npos),
                      "other platforms are refused with the reason");
        CHECK_MESSAGE((!packagePlatformSupport(other).available && !packagePlatformSupport(other).reason.empty()),
                      "other platforms are shown disabled with a reason");
    }
    CHECK_MESSAGE(packagePlatformSupport(hostPackagePlatform()).available, "the host platform packages");
}

TEST_CASE("PackageWorkflow: configure detection") {
    const std::filesystem::path project = "C:/src/zenith/projects/demo.zenith-project";
    const std::string multiConfig =
        "# cache\r\nZENITH_PACKAGE_PROJECT:FILEPATH=C:/src/zenith/projects/demo.zenith-project\r\n"
        "ZENITH_COPY_PROJECT_MODULE:BOOL=OFF\r\nCMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\r\n";
    CHECK_MESSAGE(!packageNeedsConfigure(multiConfig, project, PackageConfiguration::Release),
                  "a release tree configured for the project is reused");
    CHECK_MESSAGE(packageNeedsConfigure("", project, PackageConfiguration::Release),
                  "a tree without a cache is configured");
    CHECK_MESSAGE(packageNeedsConfigure(multiConfig, "C:/src/zenith/projects/showcase.zenith-project",
                                        PackageConfiguration::Release),
                  "a tree configured for another project is configured again");
    std::string copying = multiConfig;
    copying.replace(copying.find("BOOL=OFF"), 8, "BOOL=ON");
    CHECK_MESSAGE(packageNeedsConfigure(copying, project, PackageConfiguration::Release),
                  "a tree that copies its module beside the project is configured again");
    const std::string singleConfig = "ZENITH_PACKAGE_PROJECT:FILEPATH=C:/src/zenith/projects/demo.zenith-project\n"
                                     "ZENITH_COPY_PROJECT_MODULE:BOOL=OFF\nCMAKE_BUILD_TYPE:STRING=Release\n";
    CHECK_MESSAGE((!packageNeedsConfigure(singleConfig, project, PackageConfiguration::Release) &&
                   packageNeedsConfigure(singleConfig, project, PackageConfiguration::RelWithDebInfo)),
                  "a single-configuration tree is configured again for another configuration");
}

TEST_CASE("PackageWorkflow: runner sequence") {
    const testsupport::TempDirectory buildRoot("package-runner-sequence");
    PackagePlan plan = planPackage(demoRequest()).plan;
    plan.buildDirectory = buildRoot / "release";

    ScriptedRunner succeeding({{.lines = {{.text = "configured"}}}, {.lines = {{.text = "built"}}}, {}, {}});
    CHECK_MESSAGE((static_cast<bool>(succeeding.runner.start(plan))), "a plan starts");
    CHECK_MESSAGE((succeeding.runner.running() && succeeding.steps->started.size() == 1), "steps start one at a time");
    CHECK_MESSAGE(!succeeding.runner.start(plan), "a second package cannot start while one runs");
    const PackagePollResult finished = pollToEnd(succeeding.runner);
    CHECK_MESSAGE((finished.finished && succeeding.runner.state() == PackageState::Succeeded),
                  "every step succeeding packages");
    CHECK_MESSAGE((succeeding.steps->started ==
                   std::vector<std::string>{"Configure Release", "Build Release", "Cook textures", "Package"}),
                  "the steps run in order");
    CHECK_MESSAGE(succeeding.steps->lastWorkingDirectory == plan.buildDirectory, "steps run in the release tree");
    const auto& output = succeeding.runner.output();
    CHECK_MESSAGE((std::ranges::any_of(output, [](const BuildOutputLine& line) { return line.text == "built"; }) &&
                   output.front().text.find("[1/4] Configure Release") != std::string::npos),
                  "the output keeps every step's lines under a header");
    const std::vector<EditorDiagnostic> success =
        packageDiagnostics(succeeding.runner, std::filesystem::path("C:/out/Zenith.zip"));
    CHECK_MESSAGE((success.size() == 1 && success[0].severity == EditorDiagnosticSeverity::Info &&
                   success[0].message.find("Zenith.zip") != std::string::npos),
                  "a finished package reports its archive");

    ScriptedRunner failing({{},
                            {.state = BuildState::Failed,
                             .exitCode = 2,
                             .lines = {{.text = "a.cpp(1): error C1: broken",
                                        .severity = CompilerDiagnosticSeverity::Error,
                                        .location = EditorSourceLocation{"a.cpp", 1, 0}}}}});
    failing.runner.start(plan);
    pollToEnd(failing.runner);
    CHECK_MESSAGE((failing.runner.state() == PackageState::Failed && failing.runner.currentStep() == 1 &&
                   failing.runner.exitCode() == 2 && failing.steps->started.size() == 2),
                  "a failing step stops the package at that step");
    CHECK_MESSAGE(failing.runner.failure().find("Build Release") != std::string::npos, "the failure names its step");
    const std::vector<EditorDiagnostic> failure = packageDiagnostics(failing.runner, std::nullopt);
    CHECK_MESSAGE((failure.size() == 2 && failure[0].location &&
                   failure[1].severity == EditorDiagnosticSeverity::Error && !failure[1].action.empty()),
                  "a failed package publishes its errors and a summary");

    ScriptedRunner cancelled({{}, {}, {}, {}});
    cancelled.runner.start(plan);
    cancelled.runner.poll();
    cancelled.runner.cancel();
    pollToEnd(cancelled.runner);
    CHECK_MESSAGE((cancelled.runner.state() == PackageState::Cancelled && cancelled.steps->started.size() == 1),
                  "cancelling stops the package without starting more steps");

    ScriptedRunner unstartable({{}, {.startFails = true}});
    unstartable.runner.start(plan);
    pollToEnd(unstartable.runner);
    CHECK_MESSAGE((unstartable.runner.state() == PackageState::Failed &&
                   unstartable.runner.failure().find("no such program") != std::string::npos),
                  "a step that cannot start fails the package");

    ScriptedRunner empty({});
    CHECK_MESSAGE(!empty.runner.start({}), "an empty plan does not start");
}

struct OperatorFixture {
    Scene scene;
    AssetManager assets;
    testsupport::TempDirectory documentRoot{"package-operator"};
    EditorDocument document{documentRoot.path()};
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

TEST_CASE("PackageWorkflow: build package operator") {
    const testsupport::TempDirectory scratch("package-workflow-tests");
    const std::filesystem::path root = scratch.path();
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
    CHECK_MESSAGE((request.sourceDirectory == source.lexically_normal() &&
                   request.buildDirectory == (source / "out" / "release").lexically_normal() &&
                   request.projectFile == project.lexically_normal()),
                  "the request resolves the build directory, package directory, and project");

    ScriptedRunner scripted({{}, {}, {}, {}});
    OperatorFixture fixture;
    CHECK_MESSAGE(
        registerPackageOperators(fixture.registry, {.runner = scripted.runner, .tools = tools, .projectFile = project})
            .success,
        "file.build_package registers");
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("file.build_package"))), "file.build_package starts headless");
    CHECK_MESSAGE((scripted.runner.running() && scripted.runner.plan().steps.size() == 4 &&
                   scripted.runner.plan().projectFile == project.lexically_normal()),
                  "an unconfigured release tree is configured first, for the open project");
    CHECK_MESSAGE(!fixture.invoke("file.build_package"), "a second package does not start while one runs");

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
        CHECK_MESSAGE(panel.view() == BuildPanel::View::Package, "the Build panel shows a package when it starts");
        pollToEnd(scripted.runner);
        HeadlessImGuiFrame frame("Build");
        EditorContext context = panelFixture.context();
        panel.drawContents(context);
    }
    CHECK_MESSAGE(scripted.runner.state() == PackageState::Succeeded, "the scripted package finishes");

    const std::string otherPlatform = hostPackagePlatform() == PackagePlatform::Linux ? "windows" : "linux";
    ScriptedRunner idle({});
    OperatorFixture crossFixture;
    registerPackageOperators(crossFixture.registry, {.runner = idle.runner, .tools = tools, .projectFile = project});
    const OperatorResult cross =
        crossFixture.invoke("file.build_package", R"({"platform":")" + otherPlatform + R"("})");
    CHECK_MESSAGE((cross.status == OperatorStatus::Failed && cross.message.find("computer") != std::string::npos),
                  "another platform fails with the reason");

    OperatorFixture noProjectFixture;
    registerPackageOperators(noProjectFixture.registry, {.runner = idle.runner, .tools = tools, .projectFile = {}});
    const OperatorResult noProject = noProjectFixture.invoke("file.build_package");
    CHECK_MESSAGE(
        (noProject.status == OperatorStatus::Failed && noProject.message.find("No project") != std::string::npos),
        "packaging without a project explains why");

    EditorToolSettings elsewhere = tools;
    elsewhere.buildDirectory = (root / "not a checkout").string();
    std::filesystem::create_directories(root / "not a checkout");
    OperatorFixture noSourceFixture;
    registerPackageOperators(noSourceFixture.registry,
                             {.runner = idle.runner, .tools = elsewhere, .projectFile = project});
    const OperatorResult noSource = noSourceFixture.invoke("file.build_package");
    CHECK_MESSAGE(
        (noSource.status == OperatorStatus::Failed && noSource.message.find("CMakeLists.txt") != std::string::npos),
        "a build directory without the engine checkout explains why");
    CHECK_MESSAGE(idle.runner.state() == PackageState::Idle, "refused packages never start");

    std::ofstream(root / "Zenith-old.zip") << "old";
    std::ofstream(root / "notes.txt") << "notes";
    CHECK_MESSAGE(newestPackageArchive(root) == root / "Zenith-old.zip",
                  "the archive is found among the build outputs");
}
} // namespace

TEST_SUITE_END();
