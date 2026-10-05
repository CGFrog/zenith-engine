// The code generators: naming rules, marker insertion, generation from the
// C++ game template's real snippets into a small project, the failures that
// leave a project untouched, and the module.generate_code operator.

#include "EditorTestFixture.hpp"
#include "editor/operators/CodeOperators.hpp"
#include "editor/project/CodeGenerator.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/settings/EditorSettings.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeText(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

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
    fs::remove_all(directory);
    writeText(directory / "game.zenith-project", "{}");
    writeText(directory / "src" / "CMakeLists.txt", moduleCmake);
    writeText(directory / "src" / "GameModule.cpp", moduleSource);
    return directory / "game.zenith-project";
}

void testNames() {
    expect(snakeCaseName("MaxHealth") == "max_health" && snakeCaseName("HTTPServer") == "http_server" &&
               snakeCaseName("speed") == "speed" && snakeCaseName("Level2Boss") == "level2_boss",
           "names: snake_case splits words at case changes");
    expect(spacedDisplayName("PlayerMovement") == "Player Movement", "names: display names are spaced");
    expect(generatedTypeName(GeneratedCodeKind::Component, "Health").typeName == "HealthComponent" &&
               generatedTypeName(GeneratedCodeKind::Component, "HealthComponent").typeName == "HealthComponent" &&
               generatedTypeName(GeneratedCodeKind::System, "Spin").typeName == "SpinSystem" &&
               generatedTypeName(GeneratedCodeKind::Behaviour, "Door").typeName == "Door",
           "names: components and systems get their suffix once");
    for (const char* bad : {"", "health", "Has Space", "Bad-Name", "Component"}) {
        expect(!generatedTypeName(GeneratedCodeKind::Component, bad).error.empty(),
               std::string("names: '") + bad + "' is refused");
    }
    expect(!generatedTypeName(GeneratedCodeKind::Behaviour, "Behaviour").error.empty(),
           "names: engine type names are refused");
    expect(validateGeneratedFieldName("maxSpeed").empty() && !validateGeneratedFieldName("MaxSpeed").empty() &&
               !validateGeneratedFieldName("max_speed").empty(),
           "names: fields are camelCase");

    std::string error;
    const std::optional<std::vector<GeneratedField>> fields =
        parseGeneratedFields(" speed:float,target : entity", error);
    expect(fields && fields->size() == 2 && (*fields)[1].name == "target" &&
               (*fields)[1].type == GeneratedFieldType::Entity &&
               formatGeneratedFields(*fields) == "speed:float, target:entity",
           "fields: parse and format round trip");
    expect(parseGeneratedFields("", error) && parseGeneratedFields("", error)->empty(), "fields: empty text is none");
    expect(!parseGeneratedFields("speed", error) && !parseGeneratedFields("speed:double", error),
           "fields: a missing or unknown type is an error");
}

void testMarkers() {
    std::string text = "list(\n    A.cpp\n    # zenith:sources\n)\n";
    expect(insertAboveMarker(text, "# zenith:sources", "B.cpp") &&
               text == "list(\n    A.cpp\n    B.cpp\n    # zenith:sources\n)\n",
           "markers: a line goes above the marker, indented as it is");
    expect(insertAboveMarker(text, "# zenith:sources", "B.cpp") &&
               text == "list(\n    A.cpp\n    B.cpp\n    # zenith:sources\n)\n",
           "markers: a line already above the marker is not repeated");
    std::string windows = "a\r\n// zenith:register\r\n";
    expect(insertAboveMarker(windows, "// zenith:register", "b") && windows == "a\r\nb\r\n// zenith:register\r\n",
           "markers: CRLF files keep CRLF");
    std::string none = "nothing here\n";
    expect(!insertAboveMarker(none, "# zenith:sources", "B.cpp") && none == "nothing here\n",
           "markers: no marker leaves the text alone");
}

void testGenerate(const fs::path& root) {
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
    expect(static_cast<bool>(behaviour), "generate: a behaviour with every field type: " + behaviour.error);
    const std::string header = readText(src / "DoorOpener.hpp");
    const std::string source = readText(src / "DoorOpener.cpp");
    expect(contains(header, "class DoorOpener final : public Behaviour") &&
               contains(header, "glm::vec3 openOffset{0.0f};") && contains(header, "SceneEntityId target;") &&
               contains(header, "#include <glm/vec3.hpp>") && contains(header, "DoorOpenerKey = \"door_opener\""),
           "generate: the header declares the class, fields, includes, and key");
    expect(contains(source, ".add(\"open_speed\", &DoorOpener::openSpeed)") &&
               contains(source, "addBehaviour<DoorOpener>") && contains(source, "\"Door Opener\"") &&
               contains(source, ".category = \"Behaviours\""),
           "generate: the source reflects the fields and registers the behaviour");
    expect(behaviour.sourceFile == src / "DoorOpener.cpp" &&
               contains(readText(behaviour.sourceFile), "void registerDoorOpener(") && behaviour.sourceLine > 1,
           "generate: the IDE opens the source at its register function");

    const CodeGenerationResult component = generateModuleCode(project, snippets(),
                                                              {.kind = GeneratedCodeKind::Component,
                                                               .name = "Health",
                                                               .category = "Combat",
                                                               .fields = {{"maxHealth", GeneratedFieldType::Float}}});
    const CodeGenerationResult system = generateModuleCode(
        project, snippets(),
        {.kind = GeneratedCodeKind::System, .name = "Regenerate", .phase = SystemPhase::FixedUpdate});
    expect(component && system, "generate: a component and a system");
    expect(contains(readText(src / "HealthComponent.cpp"), ".category(\"Combat\")") &&
               contains(readText(src / "HealthComponent.hpp"), "HealthComponentKey = \"health\""),
           "generate: the component keeps its category and key");
    expect(contains(readText(src / "RegenerateSystem.hpp"), "\"game.regenerate\"") &&
               contains(readText(src / "RegenerateSystem.cpp"), "SystemPhase::FixedUpdate"),
           "generate: the system is named after the module and runs in its phase");

    expect(readText(src / "CMakeLists.txt") == "add_library(game MODULE\n"
                                               "    GameModule.cpp\n"
                                               "    DoorOpener.cpp\n"
                                               "    HealthComponent.cpp\n"
                                               "    RegenerateSystem.cpp\n"
                                               "    # zenith:sources\n"
                                               ")\n",
           "generate: the sources are listed in order above the marker");
    const std::string registration = readText(src / "GameModule.cpp");
    expect(contains(registration, "#include \"RegenerateSystem.hpp\"\n// zenith:includes") &&
               contains(registration, "    registerDoorOpener(registrar);\n    registerHealthComponent(registrar);\n"
                                      "    registerRegenerateSystem(registrar);\n    // zenith:register"),
           "generate: the includes and register calls are added in order");
    for (const auto& entry : fs::directory_iterator(src)) {
        expect(!contains(readText(entry.path()), "{{"), "generate: no token is left in " + entry.path().string());
    }
}

void testFailures(const fs::path& root) {
    const fs::path project = makeProject(root / "failures");
    const fs::path src = project.parent_path() / "src";
    expect(static_cast<bool>(generateModuleCode(project, snippets(), {.name = "Door"})), "failures: the first Door");
    const std::string cmakeBefore = readText(src / "CMakeLists.txt");
    const CodeGenerationResult again = generateModuleCode(project, snippets(), {.name = "Door"});
    expect(!again && contains(again.error, "already exists") && readText(src / "CMakeLists.txt") == cmakeBefore,
           "failures: an existing type is refused and nothing changes");

    writeText(src / "GameModule.cpp", "void zenith_module_register(ModuleRegistrar& registrar) {}\n");
    const CodeGenerationResult unmarked = generateModuleCode(project, snippets(), {.name = "Window"});
    expect(!unmarked && contains(unmarked.error, "// zenith:includes") && contains(unmarked.error, "GameModule.cpp"),
           "failures: a missing marker names the file and the marker");
    expect(!fs::exists(src / "Window.hpp") && !fs::exists(src / "Window.cpp") &&
               readText(src / "CMakeLists.txt") == cmakeBefore,
           "failures: a missing marker writes nothing");

    expect(
        !generateModuleCode(project, snippets(), {.name = "Lamp", .fields = {{"enabled", GeneratedFieldType::Bool}}}),
        "failures: a behaviour cannot redeclare enabled");
    expect(!generateModuleCode(
               project, snippets(),
               {.name = "Lamp", .fields = {{"glow", GeneratedFieldType::Float}, {"glow", GeneratedFieldType::Bool}}}),
           "failures: a field listed twice is refused");
    expect(!generateModuleCode(project, root / "no snippets", {.name = "Lamp"}), "failures: missing snippets");
    expect(!generateModuleCode(root / "missing" / "game.zenith-project", snippets(), {.name = "Lamp"}),
           "failures: a project without src/ is refused");
}

void testOperators(const fs::path& root) {
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
    tools.buildDirectory = root.string();
    ProjectBuild build(tools, fixture.diagnostics);
    GeneratedCodeKind opened = GeneratedCodeKind::System;
    int dialogs = 0;
    expect(static_cast<bool>(registerCodeOperators(fixture.operators, {.projectFile = project,
                                                                       .snippetDirectory = snippets(),
                                                                       .tools = tools,
                                                                       .build = build,
                                                                       .openDialog =
                                                                           [&](GeneratedCodeKind kind) {
                                                                               opened = kind;
                                                                               ++dialogs;
                                                                           }})),
           "operators: the code family registers");
    expect(fixture.invoke("module.new_behaviour") && dialogs == 1 && opened == GeneratedCodeKind::Behaviour,
           "operators: module.new_behaviour opens the dialog for a behaviour");
    expect(fixture.invoke("module.new_component") && opened == GeneratedCodeKind::Component,
           "operators: module.new_component opens the dialog for a component");

    const size_t diagnosticsBefore = fixture.diagnostics.entries().size();
    const OperatorResult generated =
        fixture.invoke("module.generate_code", R"({"kind":"component","name":"Ammo","fields":"count:int, max:int"})");
    expect(static_cast<bool>(generated), "operators: module.generate_code generates: " + generated.message);
    expect(fs::is_regular_file(project.parent_path() / "src" / "AmmoComponent.cpp") &&
               contains(readText(project.parent_path() / "src" / "AmmoComponent.hpp"), "int32_t max{0};"),
           "operators: the component and its fields are written");
    expect(build.startCount() == 1, "operators: generating starts a build");
    expect(fixture.diagnostics.entries().size() == diagnosticsBefore, "operators: the IDE opens without a diagnostic");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (build.running() && std::chrono::steady_clock::now() < deadline) {
        build.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const OperatorResult badFields =
        fixture.invoke("module.generate_code", R"({"kind":"component","name":"Armor","fields":"value"})");
    expect(!badFields && contains(badFields.message, "name:type"), "operators: bad field text fails with the reason");

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
    expect(static_cast<bool>(
               scriptRunner.invokeWithJson("module.edit_script", scriptContext, R"({"component":"door_opener"})")),
           "operators: Edit Script opens a behaviour's registration");
    const OperatorResult unknown =
        scriptRunner.invokeWithJson("module.edit_script", scriptContext, R"({"component":"transform"})");
    expect(!unknown && contains(unknown.message, "no behaviour"),
           "operators: Edit Script on another type fails with the reason");

    const std::string sourceFile = (project.parent_path() / "src" / "GameModule.cpp").string();
    expect(static_cast<bool>(scriptRunner.invokeWithJson("module.open_source", scriptContext,
                                                         nlohmann::json{{"file", sourceFile}, {"line", 3}}.dump())),
           "operators: Open in IDE opens a file");
    const OperatorResult noFile = scriptRunner.invokeWithJson("module.open_source", scriptContext, "{}");
    expect(!noFile && contains(noFile.message, "No file"), "operators: Open in IDE without a file fails");
    EditorToolSettings noIde;
    noIde.ideCommand.clear();
    OperatorRegistry unconfigured;
    OperatorRunner unconfiguredRunner{unconfigured, fixture.history};
    registerCodeOperators(unconfigured, {.tools = noIde, .build = build});
    const OperatorResult noCommand = unconfiguredRunner.invokeWithJson("module.open_source", scriptContext,
                                                                       nlohmann::json{{"file", sourceFile}}.dump());
    expect(!noCommand && contains(noCommand.message, "ide.command"),
           "operators: Open in IDE without an IDE command fails with the setting's name");

    OperatorRegistry noProject;
    OperatorRunner runner{noProject, fixture.history};
    registerCodeOperators(noProject, {.tools = tools, .build = build});
    OperatorContext context = fixture.operatorContext();
    expect(!noProject.find("module.generate_code")->canRun(context) &&
               !noProject.find("module.new_system")->canRun(context) &&
               !noProject.find("module.edit_script")->canRun(context),
           "operators: without a project or module the code operators are unavailable");
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_code_generator_tests";
    std::error_code error;
    fs::remove_all(root, error);

    testNames();
    testMarkers();
    testGenerate(root);
    testFailures(root);
    testOperators(root);

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " code generator test(s) failed\n";
        return 1;
    }
    std::cout << "Code generator tests passed\n";
    return 0;
}
