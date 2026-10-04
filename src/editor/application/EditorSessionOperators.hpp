#pragma once

// The operator families that start the running session's workflows. They
// reach the session only through the EditorSessionCommands interfaces in
// EditorOperatorServices, and are listed in editorOperatorFamilies().

#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/operators/Operator.hpp"

#include <functional>
#include <string>

struct ApplicationContext;

// An operator that runs a session workflow. It needs the running
// application, so it polls false in headless contexts; an empty poll only
// requires the application.
Operator applicationOperator(std::string id, std::string label, std::string description,
                             std::function<bool(const OperatorContext&)> poll,
                             std::function<void(ApplicationContext&)> run);

// file.new, file.open, file.save, file.save_as, wm.open_project,
// wm.project_hub, file.quit, edit.undo, edit.redo, edit.copy, and edit.paste
// (EditorSessionOperators.cpp).
OperatorRegistrationResult registerDocumentOperatorFamily(OperatorRegistry& registry,
                                                          const EditorOperatorServices& services);
// play.start, play.simulate, play.toggle_pause, play.step, play.stop, and the
// game view's input focus (EditorPlayback.cpp).
OperatorRegistrationResult registerPlayControlOperatorFamily(OperatorRegistry& registry,
                                                             const EditorOperatorServices& services);
// wm.search_menu and wm.keymap_editor (EditorSessionOperators.cpp).
OperatorRegistrationResult registerOperatorWindowFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services);
// Modal G/R/S transforms, Duplicate and Move, and the Delete and Rename
// popups (EditorObjectCommands.cpp).
OperatorRegistrationResult registerObjectModeOperatorFamily(OperatorRegistry& registry,
                                                            const EditorOperatorServices& services);
// module.reload (EditorModuleCommands.cpp).
OperatorRegistrationResult registerModuleOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services);
// The wm.* area and workspace operators (EditorAreas.cpp).
OperatorRegistrationResult registerAreaOperatorFamily(OperatorRegistry& registry,
                                                      const EditorOperatorServices& services);
