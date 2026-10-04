#include "PackageVerification.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/resources/ContentManifest.hpp"
#include "engine/scene/Scene.hpp"
#include "game/application/GameLaunch.hpp"

#include <algorithm>
#include <system_error>

namespace {
constexpr std::string_view sceneExtension = ".zenith-scene";

PackageVerificationResult verifyResources(const std::filesystem::path& resourceRoot) {
    const ContentValidationResult content = validateContentManifest(resourceRoot);
    if (!content) {
        std::string error = "The content manifest does not match the package:";
        for (const ContentValidationIssue& issue : content.issues) {
            error += "\n" + utf8FromPath(issue.path) + ": " + issue.message;
        }
        return {.error = error};
    }
    const std::filesystem::path shaders = resourceRoot / "shaders";
    std::error_code error;
    for (const char* shader : {"mesh.vert.spv", "mesh.frag.spv", "post.frag.spv"}) {
        if (!std::filesystem::is_regular_file(shaders / shader, error)) {
            return {.error = "Package shaders are incomplete: " + utf8FromPath((shaders / shader)) + " is missing"};
        }
    }
    return {.success = true};
}

// Every scene file under the scene root, sorted, then the startup scene when
// it lies elsewhere.
std::vector<std::filesystem::path> projectScenes(const ProjectConfig& project, std::string& error) {
    std::vector<std::filesystem::path> scenes;
    std::error_code iteration;
    if (std::filesystem::is_directory(project.sceneRoot, iteration)) {
        for (std::filesystem::recursive_directory_iterator entry(project.sceneRoot, iteration), end;
             !iteration && entry != end; entry.increment(iteration)) {
            if (entry->path().extension() == sceneExtension && entry->is_regular_file(iteration)) {
                scenes.push_back(entry->path().lexically_normal());
            }
        }
    }
    if (iteration) {
        error = "Could not list the scenes under " + utf8FromPath(project.sceneRoot) + ": " + iteration.message();
        return {};
    }
    std::sort(scenes.begin(), scenes.end());
    const std::filesystem::path startup = project.startupScene.lexically_normal();
    if (!startup.empty() && std::find(scenes.begin(), scenes.end(), startup) == scenes.end()) {
        scenes.push_back(startup);
    }
    return scenes;
}
} // namespace

PackageVerificationResult verifyProjectScenes(const ProjectConfig& project, const Gameplay& gameplay) {
    std::error_code error;
    if (!std::filesystem::is_directory(project.assetRoot, error)) {
        return {.error =
                    "Project '" + project.displayName + "' asset root is missing: " + utf8FromPath(project.assetRoot)};
    }
    std::string listError;
    const std::vector<std::filesystem::path> scenes = projectScenes(project, listError);
    if (!listError.empty()) {
        return {.error = listError};
    }
    if (scenes.empty()) {
        return {.error =
                    "Project '" + project.displayName + "' has no scenes under " + utf8FromPath(project.sceneRoot)};
    }

    PackageVerificationResult verified{.success = true};
    for (const std::filesystem::path& sceneFile : scenes) {
        // A fresh scene and asset cache per file, so one scene cannot satisfy
        // another's references.
        AssetManager assets;
        Scene scene;
        if (const ProjectConfigResult result =
                loadProjectSceneFile(project, sceneFile, assets, scene, gameplay.componentTypes);
            !result) {
            return {.error = "Scene " + utf8FromPath(sceneFile) + " does not load: " + result.error};
        }
        verified.scenes.push_back(sceneFile);
    }
    return verified;
}

PackageVerificationResult verifyPackage(const std::filesystem::path& resourceRoot, const ProjectConfig* project,
                                        const Gameplay& gameplay) {
    if (PackageVerificationResult result = verifyResources(resourceRoot); !result) {
        return result;
    }
    if (project == nullptr) {
        return {.success = true};
    }
    return verifyProjectScenes(*project, gameplay);
}
