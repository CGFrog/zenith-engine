// The session's document and operator-window operators: file commands,
// project switching, quit, undo and redo, copy and paste, the F3 search, and
// the keymap editor.

#include "EditorSessionOperators.hpp"

#include "editor/application/EditorSessionCommands.hpp"

#include <utility>
#include <vector>

namespace {
// project.settings {tab}: the tab to show (projectSettingsTab), or empty.
struct ProjectSettingsProperties {
    std::string tab;
};
} // namespace

Operator applicationOperator(std::string id, std::string label, std::string description,
                             std::function<bool(const OperatorContext&)> poll,
                             std::function<void(ApplicationContext&)> run) {
    return {.id = std::move(id),
            .label = std::move(label),
            .description = std::move(description),
            .poll = [poll = std::move(poll)](
                        const OperatorContext& context) { return context.app != nullptr && (!poll || poll(context)); },
            .execute =
                [run = std::move(run)](OperatorContext& context, OperatorProperties&) {
                    run(*context.app);
                    return OperatorResult::finished();
                }};
}

OperatorRegistrationResult registerDocumentOperatorFamily(OperatorRegistry& registry,
                                                          const EditorOperatorServices& services) {
    EditorDocumentCommands& commands = services.documentCommands;
    EditorObjectModeCommands& objectMode = services.objectModeCommands;
    const auto documentCommands = [&commands](const OperatorContext& context) {
        return commands.documentCommandsAvailable(context);
    };
    const auto projectHubAvailable = [&commands](const OperatorContext&) { return commands.projectHubAvailable(); };

    std::vector<Operator> operators;
    operators.push_back(applicationOperator("file.new", "New", "Start a new scene with the default content",
                                            documentCommands,
                                            [&commands](ApplicationContext& app) { commands.requestNew(app); }));
    operators.push_back(applicationOperator("file.open", "Open", "Open a scene file", documentCommands,
                                            [&commands](ApplicationContext& app) { commands.requestOpen(app); }));
    // Saves the document the operator runs on: a prefab window's saves its
    // prefab.
    operators.push_back({.id = "file.save",
                         .label = "Save",
                         .description = "Save the scene to its file, or the open prefab to the prefab's file",
                         .poll =
                             [&commands](const OperatorContext& context) {
                                 return context.app != nullptr && commands.editCommandsAvailable(context);
                             },
                         .execute =
                             [&commands](OperatorContext& context, OperatorProperties&) {
                                 commands.saveDocument(context);
                                 return OperatorResult::finished();
                             }});
    // A prefab saves only to its own file.
    operators.push_back(applicationOperator(
        "file.save_as", "Save As", "Save the scene to a new file",
        [&commands](const OperatorContext& context) { return commands.saveAsAvailable(context); },
        [&commands](ApplicationContext&) { commands.requestSaveAs(); }));
    operators.push_back(applicationOperator(
        "wm.open_project", "Open Project", "Close this project and choose another .zenith-project file to open",
        projectHubAvailable,
        [&commands](ApplicationContext& app) { commands.requestProjectHub(app, ProjectHubEntry::OpenProjectDialog); }));
    operators.push_back(applicationOperator(
        "wm.project_hub", "Project Hub",
        "Close this project and return to the Project Hub's recent projects and templates", projectHubAvailable,
        [&commands](ApplicationContext& app) { commands.requestProjectHub(app, ProjectHubEntry::Hub); }));
    operators.push_back(applicationOperator(
        "file.quit", "Quit", "Close the editor, asking about unsaved changes first",
        [&objectMode](const OperatorContext&) { return !objectMode.modalTransformRunning(); },
        [&commands](ApplicationContext& app) { commands.requestClose(app); }));

    operators.push_back({.id = "edit.undo",
                         .label = "Undo",
                         .description = "Undo the last change to the document",
                         .poll =
                             [&commands](const OperatorContext& context) {
                                 return context.app != nullptr && commands.editCommandsAvailable(context) &&
                                        context.editing.canUndo();
                             },
                         .execute =
                             [&commands](OperatorContext& context, OperatorProperties&) {
                                 commands.undo(context);
                                 return OperatorResult::finished();
                             }});
    operators.push_back({.id = "edit.redo",
                         .label = "Redo",
                         .description = "Redo the last undone change to the document",
                         .poll =
                             [&commands](const OperatorContext& context) {
                                 return context.app != nullptr && commands.editCommandsAvailable(context) &&
                                        context.editing.canRedo();
                             },
                         .execute =
                             [&commands](OperatorContext& context, OperatorProperties&) {
                                 commands.redo(context);
                                 return OperatorResult::finished();
                             }});
    operators.push_back({.id = "edit.copy",
                         .label = "Copy",
                         .description = "Copy the selected objects to the clipboard",
                         .poll =
                             [](const OperatorContext& context) {
                                 return context.app != nullptr && context.editMode() && !context.selection.empty();
                             },
                         .execute = [&commands](OperatorContext& context,
                                                OperatorProperties&) { return commands.copySelection(context); }});
    operators.push_back(
        {.id = "edit.paste",
         .label = "Paste Entities",
         .description = "Paste objects from the clipboard and select them",
         .flags = OperatorFlags::Undo,
         .poll = [](const OperatorContext& context) { return context.app != nullptr && context.editMode(); },
         .execute = [&commands](OperatorContext& context,
                                OperatorProperties&) { return commands.pasteClipboard(context); },
         .scope = newEntitiesScope()});
    return addOperators(registry, std::move(operators));
}

OperatorRegistrationResult registerOperatorWindowFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services) {
    EditorWindowCommands& windows = services.windowCommands;
    std::vector<Operator> operators;
    operators.push_back(applicationOperator("wm.search_menu", "Search Operators",
                                            "Find and run any available operator by name", {},
                                            [&windows](ApplicationContext&) { windows.openOperatorSearch(); }));
    operators.push_back(applicationOperator("wm.keymap_editor", "Keymap Editor",
                                            "Change, add, or reset keyboard shortcuts", {},
                                            [&windows](ApplicationContext&) { windows.openKeymapEditor(); }));
    operators.push_back(
        {.id = "project.settings",
         .label = "Project Settings",
         .description =
             "Edit the project's physics layers and collision matrix, input actions, tags, and navigation settings",
         .properties = OperatorProperties::make(
             ProjectSettingsProperties{},
             std::move(Fields<ProjectSettingsProperties>()
                           .add("tab", &ProjectSettingsProperties::tab)
                           .tooltip("The tab to show: physics_layers, input_actions, tags, or navigation"))),
         .poll = [hasProject = !services.projectFile.empty()](
                     const OperatorContext& context) { return hasProject && context.app != nullptr; },
         .execute =
             [&windows](OperatorContext& context, OperatorProperties& properties) {
                 const ProjectSettingsProperties* values = properties.get<ProjectSettingsProperties>();
                 windows.openProjectSettings(*context.app, values != nullptr ? values->tab : std::string_view{});
                 return OperatorResult::finished();
             }});
    return addOperators(registry, std::move(operators));
}
