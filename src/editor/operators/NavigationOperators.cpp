#include "NavigationOperators.hpp"

#include "editor/operators/EditorOperatorFamilies.hpp"
#include "engine/application/Application.hpp"
#include "engine/navigation/NavigationWorld.hpp"

#include <string>

namespace {
Operator bakeOperator(NavigationWorld& preview) {
    return {.id = "navigation.bake",
            .label = "Bake Navigation Mesh",
            .description = "Build a preview navigation mesh from the scene's static colliders",
            .poll = [](const OperatorContext& context) { return context.editMode() && context.app != nullptr; },
            .execute =
                [&preview](OperatorContext& context, OperatorProperties&) {
                    context.app->scene.updateWorldTransforms();
                    const NavigationResult result = preview.bake(context.app->scene, context.app->assets);
                    if (!result) {
                        return OperatorResult::failed("Navigation mesh: " + result.error);
                    }
                    return OperatorResult::finished();
                }};
}

Operator clearOperator(NavigationWorld& preview) {
    return {.id = "navigation.clear",
            .label = "Clear Navigation Mesh",
            .description = "Remove the preview navigation mesh",
            .poll = [&preview](const OperatorContext&) { return preview.baked(); },
            .execute =
                [&preview](OperatorContext&, OperatorProperties&) {
                    preview.clear();
                    return OperatorResult::finished();
                }};
}
} // namespace

OperatorRegistrationResult registerNavigationOperators(OperatorRegistry& registry,
                                                       NavigationOperatorServices services) {
    for (Operator op : {bakeOperator(services.preview), clearOperator(services.preview)}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}

OperatorRegistrationResult registerNavigationOperatorFamily(OperatorRegistry& registry,
                                                            const EditorOperatorServices& services) {
    return registerNavigationOperators(registry, {.preview = services.navigationPreview});
}
