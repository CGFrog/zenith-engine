#include "CodeOperators.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorSessionCommands.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/settings/EditorSettings.hpp"

#include <array>
#include <memory>
#include <utility>

namespace {
Fields<GenerateCodeProperties> generateCodeFields() {
    std::vector<EnumOption> phases;
    for (const SystemPhase phase : systemPhases) {
        const std::string name(systemPhaseName(phase));
        phases.push_back({.key = name, .label = name, .value = static_cast<int64_t>(phase)});
    }
    return std::move(Fields<GenerateCodeProperties>()
                         .add("kind", &GenerateCodeProperties::kind)
                         .options({{"behaviour", "Behaviour", static_cast<int64_t>(GeneratedCodeKind::Behaviour)},
                                   {"component", "Component", static_cast<int64_t>(GeneratedCodeKind::Component)},
                                   {"system", "System", static_cast<int64_t>(GeneratedCodeKind::System)}})
                         .add("name", &GenerateCodeProperties::name)
                         .add("category", &GenerateCodeProperties::category)
                         .add("phase", &GenerateCodeProperties::phase)
                         .options(std::move(phases))
                         .add("fields", &GenerateCodeProperties::fields));
}

bool canGenerate(const CodeOperatorServices& services) {
    return !services.projectFile.empty() && !services.snippetDirectory.empty();
}

OperatorResult generateCode(const CodeOperatorServices& services, OperatorContext& context,
                            const GenerateCodeProperties& properties) {
    std::string error;
    std::optional<std::vector<GeneratedField>> fields = parseGeneratedFields(properties.fields, error);
    if (!fields) {
        return OperatorResult::failed(error);
    }
    const CodeGenerationResult generated = generateModuleCode(services.projectFile, services.snippetDirectory,
                                                              {.kind = properties.kind,
                                                               .name = properties.name,
                                                               .category = properties.category,
                                                               .phase = properties.phase,
                                                               .fields = std::move(*fields)});
    if (!generated) {
        return OperatorResult::failed(generated.error);
    }
    // A failed IDE launch is reported, but the type exists and builds.
    openSourceLocation(services.tools.ideCommand, {.file = generated.sourceFile, .line = generated.sourceLine},
                       context.editing.diagnostics());
    services.build.request();
    return OperatorResult::finished();
}

Operator editScriptOperator(const std::shared_ptr<const CodeOperatorServices>& services) {
    const auto source = [services](const OperatorProperties& properties) -> std::optional<EditorSourceLocation> {
        const auto* values = properties.get<EditScriptProperties>();
        if (values == nullptr || !services->behaviourSource) {
            return std::nullopt;
        }
        return services->behaviourSource(values->component);
    };
    return {.id = "module.edit_script",
            .label = "Edit Script",
            .description = "Open the behaviour's source in the IDE, where the game module registers it",
            .properties = OperatorProperties::make(
                EditScriptProperties{},
                std::move(Fields<EditScriptProperties>().add("component", &EditScriptProperties::component))),
            .poll = [services](const OperatorContext&) { return static_cast<bool>(services->behaviourSource); },
            .execute =
                [services, source](OperatorContext&, OperatorProperties& properties) {
                    const std::optional<EditorSourceLocation> location = source(properties);
                    if (!location) {
                        return OperatorResult::failed(
                            "The game module has no behaviour with that key; build and reload it first");
                    }
                    const ChildProcessResult opened = openInExternalEditor(services->tools.ideCommand, *location);
                    if (!opened) {
                        return OperatorResult::failed("Could not open the IDE (ide.command): " + opened.error);
                    }
                    return OperatorResult::finished();
                }};
}

Operator dialogOperator(const std::shared_ptr<const CodeOperatorServices>& services, GeneratedCodeKind kind,
                        std::string id, std::string label, std::string description) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .poll =
                [services](const OperatorContext&) {
                    return canGenerate(*services) && static_cast<bool>(services->openDialog);
                },
            .execute =
                [services, kind](OperatorContext&, OperatorProperties&) {
                    services->openDialog(kind);
                    return OperatorResult::finished();
                }};
}
} // namespace

OperatorRegistrationResult registerCodeOperators(OperatorRegistry& registry, CodeOperatorServices services) {
    const auto shared = std::make_shared<const CodeOperatorServices>(std::move(services));
    std::array operators{
        dialogOperator(shared, GeneratedCodeKind::Behaviour, "module.new_behaviour", "New Behaviour...",
                       "Write a new Behaviour (per-entity logic) into the game module, open it in the IDE, and build"),
        dialogOperator(shared, GeneratedCodeKind::Component, "module.new_component", "New Component...",
                       "Write a new component type into the game module, open it in the IDE, and build"),
        dialogOperator(shared, GeneratedCodeKind::System, "module.new_system", "New System...",
                       "Write a new system into the game module, open it in the IDE, and build"),
        Operator{.id = "module.generate_code",
                 .label = "Generate Code",
                 .description = "Write a behaviour, component, or system into the game module's src folder, "
                                "open it in the IDE, and build",
                 .properties = OperatorProperties::make(GenerateCodeProperties{}, generateCodeFields()),
                 .poll = [shared](const OperatorContext&) { return canGenerate(*shared); },
                 .execute =
                     [shared](OperatorContext& context, OperatorProperties& properties) {
                         const auto* values = properties.get<GenerateCodeProperties>();
                         if (values == nullptr) {
                             return OperatorResult::failed("Operator properties have the wrong type");
                         }
                         return generateCode(*shared, context, *values);
                     }},
        editScriptOperator(shared),
    };
    for (Operator& op : operators) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}

OperatorRegistrationResult registerCodeOperatorFamily(OperatorRegistry& registry,
                                                      const EditorOperatorServices& services) {
    EditorWindowCommands& windows = services.windowCommands;
    return registerCodeOperators(
        registry, {.projectFile = services.projectFile,
                   .snippetDirectory = services.codeSnippetDirectory,
                   .tools = services.tools,
                   .build = services.build,
                   .openDialog = [&windows](GeneratedCodeKind kind) { windows.openNewCodeDialog(kind); },
                   .behaviourSource =
                       [&modules = services.modules](std::string_view key) { return modules.behaviourSource(key); }});
}
