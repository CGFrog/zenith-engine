#pragma once

// zenith --verify-package: checks the package's assets and that every scene loads.

#include "engine/module/Gameplay.hpp"
#include "engine/project/ProjectConfig.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct PackageVerificationResult {
    bool success{false};
    std::string error;
    // The scene files that loaded, in the order they were checked.
    std::vector<std::filesystem::path> scenes;

    explicit operator bool() const {
        return success;
    }
};

// Checks that project's asset root exists and that every .zenith-scene under
// its scene root, plus its startup scene wherever it lies, loads with
// gameplay's component types, each into a fresh scene with its own asset
// cache. Fails when the project has no scene at all.
PackageVerificationResult verifyProjectScenes(const ProjectConfig& project, const Gameplay& gameplay);

// Checks a release package without a GPU (zenith --verify-package): its
// content manifest matches the files, its compiled shaders are present, and,
// for project, verifyProjectScenes. The caller has already loaded gameplay
// (for a project with a game module, the module itself).
PackageVerificationResult verifyPackage(const std::filesystem::path& resourceRoot, const ProjectConfig* project,
                                        const Gameplay& gameplay);
