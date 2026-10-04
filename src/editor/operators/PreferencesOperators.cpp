#include "PreferencesOperators.hpp"

#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/panels/PreferencesWindow.hpp"

OperatorRegistrationResult registerPreferencesOperators(OperatorRegistry& registry, PreferencesWindow& preferences) {
    return registry.add({.id = "wm.preferences",
                         .label = "Preferences",
                         .description = "Tune the theme, UI scale, keymap, IDE, autosave, and build settings",
                         .execute = [&preferences](OperatorContext&, OperatorProperties&) {
                             preferences.open();
                             return OperatorResult::finished();
                         }});
}

OperatorRegistrationResult registerPreferencesOperatorFamily(OperatorRegistry& registry,
                                                             const EditorOperatorServices& services) {
    return registerPreferencesOperators(registry, services.preferences);
}
