// The session's document and operator-window operators: file commands,
// project switching, quit, undo and redo, copy and paste, the F3 search, and
// the keymap editor.

#include "EditorSessionOperators.hpp"

#include "editor/application/EditorSessionCommands.hpp"
#include "editor/document/CommandHistory.hpp"

#include <utility>
#include <vector>

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
    CommandHistory& history = services.history;
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
    operators.push_back(applicationOperator("file.save", "Save", "Save the scene to its file", documentCommands,
                                            [&commands](ApplicationContext& app) { commands.saveDocument(app); }));
    operators.push_back(applicationOperator("file.save_as", "Save As", "Save the scene to a new file", documentCommands,
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

    operators.push_back(applicationOperator(
        "edit.undo", "Undo", "Undo the last scene change",
        [&commands, &history](const OperatorContext& context) {
            return commands.documentCommandsAvailable(context) && history.canUndo();
        },
        [&commands](ApplicationContext& app) { commands.undo(app); }));
    operators.push_back(applicationOperator(
        "edit.redo", "Redo", "Redo the last undone scene change",
        [&commands, &history](const OperatorContext& context) {
            return commands.documentCommandsAvailable(context) && history.canRedo();
        },
        [&commands](ApplicationContext& app) { commands.redo(app); }));
    operators.push_back({.id = "edit.copy",
                         .label = "Copy",
                         .description = "Copy the selected objects to the clipboard",
                         .poll =
                             [](const OperatorContext& context) {
                                 return context.app != nullptr && context.editMode() && !context.selection.empty();
                             },
                         .execute = [&commands](OperatorContext& context,
                                                OperatorProperties&) { return commands.copySelection(*context.app); }});
    operators.push_back(
        {.id = "edit.paste",
         .label = "Paste Entities",
         .description = "Paste objects from the clipboard and select them",
         .flags = OperatorFlags::Undo,
         .poll = [](const OperatorContext& context) { return context.app != nullptr && context.editMode(); },
         .execute = [&commands](OperatorContext& context,
                                OperatorProperties&) { return commands.pasteClipboard(*context.app); }});
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
    return addOperators(registry, std::move(operators));
}
