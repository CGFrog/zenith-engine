#pragma once

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/project/CodeGenerator.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class ProjectBuild;
struct EditorOperatorServices;
struct EditorToolSettings;

// Properties of module.generate_code: the request generateModuleCode runs,
// with the fields as text ("speed:float, target:entity", see
// parseGeneratedFields).
struct GenerateCodeProperties {
    GeneratedCodeKind kind{GeneratedCodeKind::Behaviour};
    std::string name;
    std::string category;
    SystemPhase phase{SystemPhase::Update};
    std::string fields;
};

struct CodeOperatorServices {
    // The open .zenith-project; empty when the editor runs without one.
    std::filesystem::path projectFile;
    // The C++ game template's snippets folder; empty when the editor found
    // no template.
    std::filesystem::path snippetDirectory;
    // ide.command opens the new source.
    const EditorToolSettings& tools;
    ProjectBuild& build;
    // Opens the New Behaviour, Component, or System dialog, which runs
    // module.generate_code. May be empty (headless).
    std::function<void(GeneratedCodeKind)> openDialog;
    // Where the loaded game module registered the behaviour with a component
    // key (GameModuleHost::behaviourSource). May be empty.
    std::function<std::optional<EditorSourceLocation>(std::string_view)> behaviourSource;
};

// Properties of module.edit_script.
struct EditScriptProperties {
    // The behaviour's component key.
    std::string component;
};

// Properties of module.open_source.
struct OpenSourceProperties {
    // The file to open: UTF-8, absolute or relative to the editor's working
    // directory.
    std::string file;
    // 1-based; 0 opens at the top.
    uint32_t line{0};
};

// The code generators and IDE launchers:
//
//   module.new_behaviour, module.new_component, module.new_system
//       open the dialog for that kind
//   module.generate_code {kind, name, category, phase, fields}
//       writes the type into the project's src/ (generateModuleCode), opens
//       its source in the IDE, and builds (build.run's queueing applies)
//   module.edit_script {component}
//       opens the IDE where the loaded module registered that behaviour
//   module.open_source {file, line}
//       opens any file in the IDE (ide.command); the Asset Browser uses it
//       for scripts
//
// The generators need a project and the template's snippets; edit_script
// needs a loaded behaviour. generate_code, edit_script, and open_source need
// no window, so they run headless.
OperatorRegistrationResult registerCodeOperators(OperatorRegistry& registry, CodeOperatorServices services);

OperatorRegistrationResult registerCodeOperatorFamily(OperatorRegistry& registry,
                                                      const EditorOperatorServices& services);
