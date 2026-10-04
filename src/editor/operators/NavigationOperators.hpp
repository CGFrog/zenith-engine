#pragma once

#include "editor/operators/OperatorRegistry.hpp"

class NavigationWorld;
struct EditorOperatorServices;

// Bakes a preview navigation mesh of the edited scene, which every viewport
// draws while it holds one. The preview is editor state: baking records no
// undo step and changes neither the scene nor the document, and games bake
// their own mesh when gameplay starts.
//
//   navigation.bake   bake the edit scene's static colliders (Edit mode)
//   navigation.clear  remove the preview
struct NavigationOperatorServices {
    NavigationWorld& preview;
};

OperatorRegistrationResult registerNavigationOperators(OperatorRegistry& registry, NavigationOperatorServices services);

OperatorRegistrationResult registerNavigationOperatorFamily(OperatorRegistry& registry,
                                                            const EditorOperatorServices& services);
