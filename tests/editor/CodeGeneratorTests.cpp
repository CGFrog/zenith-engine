// The code generators: naming rules, marker insertion, generation from the
// C++ game template's real snippets into a small project, the failures that
// leave a project untouched, and the module.generate_code operator.

#include "EditorTestFixture.hpp"
#include "editor/operators/CodeOperators.hpp"
#include "editor/project/CodeGenerator.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/settings/EditorSettings.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

TEST_SUITE_BEGIN("CodeGenerator");

namespace {

using testsupport::readText;
using testsupport::writeText;

bool contains(std::string_view text, std::string_view fragment) {
    return text.find(fragment) != std::string_view::npos;
}

fs::path snippets() {
    return fs::path(ZENITH_TEMPLATE_DIR) / cppGameTemplateName / templateSnippetDirectoryName;
}

constexpr std::string_view moduleCmake = "add_library(game MODULE\n"
                                         "    GameModule.cpp\n"
                                         "    # zenith:sources\n"
                                         ")\n";
constexpr std::string_view moduleSource = "#include \"SpinnerGameplay.hpp\"\n"
                                          "// zenith:includes\n"
                                          "\n"
                                          "void zenith_module_register(ModuleRegistrar& registrar) {\n"
                                          "    registerSpinnerGameplay(registrar);\n"
                                          "    // zenith:register\n"
                                          "}\n";

// A project with the template's markers; returns its project file.
fs::path makeProject(const fs::path& directory) {
    writeText(directory / "game.zenith-project", "{}");
    writeText(directory / "src" / "CMakeLists.txt", moduleCmake);
    writeText(directory / "src" / "GameModule.cpp", moduleSource);
    return directory / "game.zenith-project";
}

TEST_CASE("CodeGenerator: names") {
    CHECK_MESSAGE((snakeCaseName("MaxHealth") == "max_health" && snakeCaseName("HTTPServer") == "http_server" &&
                   snakeCaseName("speed") == "speed" && snakeCaseName("Level2Boss") == "level2_boss"),
                  "names: snake_case splits words at case changes");
    CHECK_MESSAGE(spacedDisplayName("PlayerMovement") == "Player Movement", "names: display names are spaced");
    CHECK_MESSAGE((generatedTypeName(GeneratedCodeKind::Component, "Health").typeName == "HealthComponent" &&
                   generatedTypeName(GeneratedCodeKind::Component, "HealthComponent").typeName == "HealthComponent" &&
                   generatedTypeName(GeneratedCodeKind::System, "Spin").typeName == "SpinSystem" &&
                   generatedTypeName(GeneratedCodeKind::Behaviour, "Door").typeName == "Door"),
                  "names: components and systems get their suffix once");
    for (const char* bad : {"", "health", "Has Space", "Bad-Name", "Component"}) {
        CHECK_MESSAGE(!generatedTypeName(GeneratedCodeKind::Component, bad).error.empty(),
                      (std::string("names: '") + bad + "' is refused"));
    }
    CHECK_MESSAGE(!generatedTypeName(GeneratedCodeKind::Behaviour, "Behaviour").error.empty(),
                  "names: engine type names are refused");
    CHECK_MESSAGE((validateGeneratedFieldName("maxSpeed").empty() && !validateGeneratedFieldName("MaxSpeed").empty() &&
                   !validateGeneratedFieldName("max_speed").empty()),
                  "names: fields are camelCase");

    std::string error;
    const std::optional<std::vector<GeneratedField>> fields =
        parseGeneratedFields(" speed:float,target : entity", error);
    CHECK_MESSAGE((fields && fields->size() == 2 && (*fields)[1].name == "target" &&
                   (*fields)[1].type == GeneratedFieldType::Entity &&
                   formatGeneratedFields(*fields) == "speed:float, target:entity"),
                  "fields: parse and format round trip");
    CHECK_MESSAGE((parseGeneratedFields("", error) && parseGeneratedFields("", error)->empty()),
                  "fields: empty text is none");
    CHECK_MESSAGE((!parseGeneratedFields("speed", error) && !parseGeneratedFields("speed:double", error)),
                  "fields: a missing or unknown type is an error");
}

TEST_CASE("CodeGenerator: markers") {
    std::string text = "list(\n    A.cpp\n    # zenith:sources\n)\n";
    CHECK_MESSAGE((insertAboveMarker(text, "# zenith:sources", "B.cpp") &&
                   text == "list(\n    A.cpp\n    B.cpp\n    # zenith:sources\n)\n"),
                  "markers: a line goes above the marker, indented as it is");
    CHECK_MESSAGE((insertAboveMarker(text, "# zenith:sources", "B.cpp") &&
                   text == "list(\n    A.cpp\n    B.cpp\n    # zenith:sources\n)\n"),
                  "markers: a line already above the marker is not repeated");
    std::string windows = "a\r\n// zenith:register\r\n";
    CHECK_MESSAGE(
        (insertAboveMarker(windows, "// zenith:register", "b") && windows == "a\r\nb\r\n// zenith:register\r\n"),
        "markers: CRLF files keep CRLF");
    std::string none = "nothing here\n";
    CHECK_MESSAGE((!insertAboveMarker(none, "# zenith:sources", "B.cpp") && none == "nothing here\n"),
                  "markers: no marker leaves the text alone");
}

TEST_CASE("CodeGenerator: generate") {
    const testsupport::TempDirectory root("code-generator-generate");
    const fs::path project = makeProject(root / "generate");
    const fs::path src = project.parent_path() / "src";
    const CodeGenerationResult behaviour = generateModuleCode(project, snippets(),
                                                              {.kind = GeneratedCodeKind::Behaviour,
                                                               .name = "DoorOpener",
                                                               .fields = {{"openSpeed", GeneratedFieldType::Float},
                                                                          {"locked", GeneratedFieldType::Bool},
                                                                          {"uses", GeneratedFieldType::Int},
                                                                          {"openOffset", GeneratedFieldType::Vec3},
                                                                          {"sound", GeneratedFieldType::String},
                                                                          {"target", GeneratedFieldType::Entity}}});
    CHECK_MESSAGE(static_cast<bool>(behaviour), ("generate: a behaviour with every field type: " + behaviour.error));
    const std::string header = readText(src / "DoorOpener.hpp");
    const std::string source = readText(src / "DoorOpener.cpp");
    CHECK_MESSAGE((contains(header, "class DoorOpener final : public Behaviour") &&
                   contains(header, "glm::vec3 openOffset{0.0f};") && contains(header, "SceneEntityId target;") &&
                   contains(header, "#include <glm/vec3.hpp>") && contains(header, "DoorOpenerKey = \"door_opener\"")),
                  "generate: the header declares the class, fields, includes, and key");
    CHECK_MESSAGE((contains(source, ".add(\"open_speed\", &DoorOpener::openSpeed)") &&
                   contains(source, "addBehaviour<DoorOpener>") && contains(source, "\"Door Opener\"") &&
                   contains(source, ".category = \"Behaviours\"")),
                  "generate: the source reflects the fields and registers the behaviour");
    CHECK_MESSAGE((behaviour.sourceFile == src / "DoorOpener.cpp" &&
                   contains(readText(behaviour.sourceFile), "void registerDoorOpener(") && behaviour.sourceLine > 1),
                  "generate: the IDE opens the source at its register function");

    const CodeGenerationResult component = generateModuleCode(project, snippets(),
                                                              {.kind = GeneratedCodeKind::Component,
                                                               .name = "Health",
                                                               .category = "Combat",
                                                               .fields = {{"maxHealth", GeneratedFieldType::Float}}});
    const CodeGenerationResult system = generateModuleCode(
        project, snippets(),
        {.kind = GeneratedCodeKind::System, .name = "Regenerate", .phase = SystemPhase::FixedUpdate});
    CHECK_MESSAGE((component && system), "generate: a component and a system");
    CHECK_MESSAGE((contains(readText(src / "HealthComponent.cpp"), ".category(\"Combat\")") &&
                   contains(readText(src / "HealthComponent.hpp"), "HealthComponentKey = \"health\"")),
                  "generate: the component keeps its category and key");
    CHECK_MESSAGE((contains(readText(src / "RegenerateSystem.hpp"), "\"game.regenerate\"") &&
                   contains(readText(src / "RegenerateSystem.cpp"), "SystemPhase::FixedUpdate")),
                  "generate: the system is named after the module and runs in its phase");

    CHECK_MESSAGE(readText(src / "CMakeLists.txt") == "add_library(game MODULE\n"
                                                      "    GameModule.cpp\n"
                                                      "    DoorOpener.cpp\n"
                                                      "    HealthComponent.cpp\n"
                                                      "    RegenerateSystem.cpp\n"
                                                      "    # zenith:sources\n"
                                                      ")\n",
                  "generate: the sources are listed in order above the marker");
    const std::string registration = readText(src / "GameModule.cpp");
    CHECK_MESSAGE(
        (contains(registration, "#include \"RegenerateSystem.hpp\"\n// zenith:includes") &&
         contains(registration, "    registerDoorOpener(registrar);\n    registerHealthComponent(registrar);\n"
                                "    registerRegenerateSystem(registrar);\n    // zenith:register")),
        "generate: the includes and register calls are added in order");
    for (const auto& entry : fs::directory_iterator(src)) {
        CHECK_MESSAGE(!contains(readText(entry.path()), "{{"),
                      ("generate: no token is left in " + entry.path().string()));
    }
}

TEST_CASE("CodeGenerator: failures") {
    const testsupport::TempDirectory root("code-generator-failures");
    const fs::path project = makeProject(root / "failures");
    const fs::path src = project.parent_path() / "src";
    CHECK_MESSAGE(static_cast<bool>(generateModuleCode(project, snippets(), {.name = "Door"})),
                  "failures: the first Door");
    const std::string cmakeBefore = readText(src / "CMakeLists.txt");
    const CodeGenerationResult again = generateModuleCode(project, snippets(), {.name = "Door"});
    CHECK_MESSAGE(
        (!again && contains(again.error, "already exists") && readText(src / "CMakeLists.txt") == cmakeBefore),
        "failures: an existing type is refused and nothing changes");

    writeText(src / "GameModule.cpp", "void zenith_module_register(ModuleRegistrar& registrar) {}\n");
    const CodeGenerationResult unmarked = generateModuleCode(project, snippets(), {.name = "Window"});
    CHECK_MESSAGE(
        (!unmarked && contains(unmarked.error, "// zenith:includes") && contains(unmarked.error, "GameModule.cpp")),
        "failures: a missing marker names the file and the marker");
    CHECK_MESSAGE((!fs::exists(src / "Window.hpp") && !fs::exists(src / "Window.cpp") &&
                   readText(src / "CMakeLists.txt") == cmakeBefore),
                  "failures: a missing marker writes nothing");

    CHECK_MESSAGE(
        !generateModuleCode(project, snippets(), {.name = "Lamp", .fields = {{"enabled", GeneratedFieldType::Bool}}}),
        "failures: a behaviour cannot redeclare enabled");
    CHECK_MESSAGE(
        !generateModuleCode(
            project, snippets(),
            {.name = "Lamp", .fields = {{"glow", GeneratedFieldType::Float}, {"glow", GeneratedFieldType::Bool}}}),
        "failures: a field listed twice is refused");
    CHECK_MESSAGE(!generateModuleCode(project, root / "no snippets", {.name = "Lamp"}), "failures: missing snippets");
    CHECK_MESSAGE(!generateModuleCode(root / "missing" / "game.zenith-project", snippets(), {.name = "Lamp"}),
                  "failures: a project without src/ is refused");
}

TEST_CASE("CodeGenerator: operators") {
    const testsupport::TempDirectory root("code-generator-operators");
    const fs::path project = makeProject(root / "operators");
    EditorTestFixture fixture;
    EditorToolSettings tools;
    // The opener is cmake itself, so the IDE launch runs without an IDE.
    tools.ideCommand = std::string("\"") + ZENITH_TEST_CMAKE + "\" -E echo {file}:{line}";
#ifdef _WIN32
    tools.buildCommand = "exit /b 0";
#else
    tools.buildCommand = "exit 0";
#endif
    tools.buildDirectory = root.path().string();
    ProjectBuild build(tools, fixture.diagnostics);
    GeneratedCodeKind opened = GeneratedCodeKind::System;
    int dialogs = 0;
    CHECK_MESSAGE(static_cast<bool>(registerCodeOperators(fixture.operators, {.projectFile = project,
                                                                              .snippetDirectory = snippets(),
                                                                              .tools = tools,
                                                                              .build = build,
                                                                              .openDialog =
                                                                                  [&](GeneratedCodeKind kind) {
                                                                                      opened = kind;
                                                                                      ++dialogs;
                                                                                  }})),
                  "operators: the code family registers");
    CHECK_MESSAGE((fixture.invoke("module.new_behaviour") && dialogs == 1 && opened == GeneratedCodeKind::Behaviour),
                  "operators: module.new_behaviour opens the dialog for a behaviour");
    CHECK_MESSAGE((fixture.invoke("module.new_component") && opened == GeneratedCodeKind::Component),
                  "operators: module.new_component opens the dialog for a component");

    const size_t diagnosticsBefore = fixture.diagnostics.entries().size();
    const OperatorResult generated =
        fixture.invoke("module.generate_code", R"({"kind":"component","name":"Ammo","fields":"count:int, max:int"})");
    CHECK_MESSAGE(static_cast<bool>(generated), ("operators: module.generate_code generates: " + generated.message));
    CHECK_MESSAGE((fs::is_regular_file(project.parent_path() / "src" / "AmmoComponent.cpp") &&
                   contains(readText(project.parent_path() / "src" / "AmmoComponent.hpp"), "int32_t max{0};")),
                  "operators: the component and its fields are written");
    CHECK_MESSAGE(build.startCount() == 1, "operators: generating starts a build");
    CHECK_MESSAGE(fixture.diagnostics.entries().size() == diagnosticsBefore,
                  "operators: the IDE opens without a diagnostic");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (build.running() && std::chrono::steady_clock::now() < deadline) {
        build.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const OperatorResult badFields =
        fixture.invoke("module.generate_code", R"({"kind":"component","name":"Armor","fields":"value"})");
    CHECK_MESSAGE((!badFields && contains(badFields.message, "name:type")),
                  "operators: bad field text fails with the reason");

    OperatorRegistry scripts;
    OperatorRunner scriptRunner{scripts, fixture.history};
    registerCodeOperators(
        scripts, {.tools = tools,
                  .build = build,
                  .behaviourSource = [&](std::string_view key) -> std::optional<EditorSourceLocation> {
                      if (key != "door_opener") {
                          return std::nullopt;
                      }
                      return EditorSourceLocation{.file = project.parent_path() / "src" / "DoorOpener.cpp", .line = 12};
                  }});
    OperatorContext scriptContext = fixture.operatorContext();
    CHECK_MESSAGE(static_cast<bool>(scriptRunner.invokeWithJson("module.edit_script", scriptContext,
                                                                R"({"component":"door_opener"})")),
                  "operators: Edit Script opens a behaviour's registration");
    const OperatorResult unknown =
        scriptRunner.invokeWithJson("module.edit_script", scriptContext, R"({"component":"transform"})");
    CHECK_MESSAGE((!unknown && contains(unknown.message, "no behaviour")),
                  "operators: Edit Script on another type fails with the reason");

    const std::string sourceFile = (project.parent_path() / "src" / "GameModule.cpp").string();
    CHECK_MESSAGE(static_cast<bool>(scriptRunner.invokeWithJson(
                      "module.open_source", scriptContext, nlohmann::json{{"file", sourceFile}, {"line", 3}}.dump())),
                  "operators: Open in IDE opens a file");
    const OperatorResult noFile = scriptRunner.invokeWithJson("module.open_source", scriptContext, "{}");
    CHECK_MESSAGE((!noFile && contains(noFile.message, "No file")), "operators: Open in IDE without a file fails");
    EditorToolSettings noIde;
    noIde.ideCommand.clear();
    OperatorRegistry unconfigured;
    OperatorRunner unconfiguredRunner{unconfigured, fixture.history};
    registerCodeOperators(unconfigured, {.tools = noIde, .build = build});
    const OperatorResult noCommand = unconfiguredRunner.invokeWithJson("module.open_source", scriptContext,
                                                                       nlohmann::json{{"file", sourceFile}}.dump());
    CHECK_MESSAGE((!noCommand && contains(noCommand.message, "ide.command")),
                  "operators: Open in IDE without an IDE command fails with the setting's name");

    OperatorRegistry noProject;
    OperatorRunner runner{noProject, fixture.history};
    registerCodeOperators(noProject, {.tools = tools, .build = build});
    OperatorContext context = fixture.operatorContext();
    CHECK_MESSAGE((!noProject.find("module.generate_code")->canRun(context) &&
                   !noProject.find("module.new_system")->canRun(context) &&
                   !noProject.find("module.edit_script")->canRun(context)),
                  "operators: without a project or module the code operators are unavailable");
}
} // namespace

TEST_SUITE_END();
