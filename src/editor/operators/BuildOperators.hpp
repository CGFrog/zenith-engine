#pragma once

#include "editor/operators/OperatorRegistry.hpp"

class ProjectBuild;
struct EditorOperatorServices;

// Starts and stops the session's build (ProjectBuild), which the Build panel
// shows. Neither needs a window, so both run headless.
//
//   build.run     start a build (Ctrl B); while one runs, queue one more
//   build.cancel  cancel the running build and any queued one
struct BuildOperatorServices {
    ProjectBuild& build;
};

OperatorRegistrationResult registerBuildOperators(OperatorRegistry& registry, BuildOperatorServices services);

OperatorRegistrationResult registerBuildOperatorFamily(OperatorRegistry& registry,
                                                       const EditorOperatorServices& services);
