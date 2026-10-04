#include "EditorOperatorFamilies.hpp"

#include "editor/application/EditorSessionOperators.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/BuildOperators.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/operators/EyedropperOperators.hpp"
#include "editor/operators/NavigationOperators.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/PackageOperators.hpp"
#include "editor/operators/PlayOperators.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "editor/operators/PreferencesOperators.hpp"
#include "editor/panels/ViewportOperators.hpp"
#include "editor/viewport/Cursor3D.hpp"

#include <array>

namespace {
// One line per family; a new family appends its line. Each adapter lives
// next to its family's operators.
constexpr std::array operatorFamilies{
    OperatorFamily{"document", registerDocumentOperatorFamily},
    OperatorFamily{"play control", registerPlayControlOperatorFamily},
    OperatorFamily{"operator windows", registerOperatorWindowFamily},
    OperatorFamily{"object mode", registerObjectModeOperatorFamily},
    OperatorFamily{"viewport", registerViewportOperatorFamily},
    OperatorFamily{"3D cursor", registerCursorOperatorFamily},
    OperatorFamily{"module", registerModuleOperatorFamily},
    OperatorFamily{"area", registerAreaOperatorFamily},
    OperatorFamily{"preferences", registerPreferencesOperatorFamily},
    OperatorFamily{"package", registerPackageOperatorFamily},
    OperatorFamily{"object", registerObjectOperatorFamily},
    OperatorFamily{"eyedropper", registerEyedropperOperatorFamily},
    OperatorFamily{"component", registerComponentOperatorFamily},
    OperatorFamily{"menu", registerMenuOperatorFamily},
    OperatorFamily{"prefab", registerPrefabOperatorFamily},
    OperatorFamily{"play", registerPlayOperatorFamily},
    OperatorFamily{"navigation", registerNavigationOperatorFamily},
    OperatorFamily{"build", registerBuildOperatorFamily},
};
} // namespace

std::span<const OperatorFamily> editorOperatorFamilies() {
    return operatorFamilies;
}
