#include "BuildOperators.hpp"

#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/project/ProjectBuild.hpp"

namespace {
Operator runOperator(ProjectBuild& build) {
    return {.id = "build.run",
            .label = "Build",
            .description = "Build the game module; a successful build hot reloads it in Edit mode",
            .execute = [&build](OperatorContext&, OperatorProperties&) {
                if (build.running()) {
                    build.request();
                    return OperatorResult::finished();
                }
                if (const ChildProcessResult started = build.start(); !started) {
                    return OperatorResult::failed("Could not start the build: " + started.error);
                }
                return OperatorResult::finished();
            }};
}

Operator cancelOperator(ProjectBuild& build) {
    return {.id = "build.cancel",
            .label = "Cancel Build",
            .description = "Stop the running build",
            .poll = [&build](const OperatorContext&) { return build.running(); },
            .execute =
                [&build](OperatorContext&, OperatorProperties&) {
                    build.cancel();
                    return OperatorResult::finished();
                }};
}
} // namespace

OperatorRegistrationResult registerBuildOperators(OperatorRegistry& registry, BuildOperatorServices services) {
    if (OperatorRegistrationResult result = registry.add(runOperator(services.build)); !result) {
        return result;
    }
    return registry.add(cancelOperator(services.build));
}

OperatorRegistrationResult registerBuildOperatorFamily(OperatorRegistry& registry,
                                                       const EditorOperatorServices& services) {
    return registerBuildOperators(registry, {.build = services.build});
}
