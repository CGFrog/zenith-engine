// The entry points through which the Zenith editor (or a game executable)
// loads {{PROJECT_NAME}}'s gameplay. ZENITH_GAME_MODULE defines the module's
// name, version, and compatibility checks; zenith_module_register adds the
// game's component types and systems.

#include "SpinnerGameplay.hpp"
// zenith:includes (the editor's code generators add their headers above)

#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"

ZENITH_GAME_MODULE("{{PROJECT_IDENTIFIER}}", "0.1.0");

void zenith_module_register(ModuleRegistrar& registrar) {
    registerSpinnerGameplay(registrar);
    // zenith:register (the editor's code generators add their types above)
}

// Editor tools for the game (docs/EDITOR_EXTENSIONS.md in the Zenith source
// tree). Uncomment to add a sidebar tab to every 3D viewport that shows how
// many entities are selected and runs an editor operator:
//
// #include "editor/modules/EditorModuleApi.hpp"
//
// #include <imgui.h>
//
// ZENITH_EDITOR_MODULE_REGISTER {
//     registrar.addSidebarTab({.title = "{{PROJECT_NAME}}", .draw = [](EditorModulePanelContext& context) {
//                                  ImGui::Text("%zu selected", context.selection.size());
//                                  if (ImGui::Button("Duplicate")) {
//                                      context.invokeOperator("object.duplicate");
//                                  }
//                              }});
// }
