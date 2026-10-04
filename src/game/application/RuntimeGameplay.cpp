#include "RuntimeGameplay.hpp"

#include "game/application/LinkedGameplay.hpp"

#include <system_error>

GameplayResult loadRuntimeGameplay(const ProjectConfig* project, GameModuleLoading loading, RuntimeGameplay& runtime) {
    if (project == nullptr || project->gameModule.empty()) {
        Gameplay linked;
        if (GameplayResult result = linkDemoGameplay(linked); !result) {
            return result;
        }
        runtime.gameplay = std::move(linked);
        runtime.module.reset();
        runtime.presentation = GamePresentation::Demo;
        return {.success = true};
    }

    std::unique_ptr<GameModule> module;
    if (GameModuleResult result = GameModule::load(project->gameModule, {.loading = loading}, module); !result) {
        return {.error = "Project '" + project->displayName + "' game module: " + result.error};
    }
    // Replaced in an order that releases the previous gameplay before the
    // module whose code it may hold.
    runtime.gameplay = gameplayFromModule(*module);
    runtime.presentation = module->name() == demoGameModuleName ? GamePresentation::Demo : GamePresentation::Project;
    runtime.module = std::move(module);
    return {.success = true};
}

GameModuleLoading runtimeModuleLoading(const std::filesystem::path& module, const std::filesystem::path& resourceRoot) {
    std::error_code error;
    // Only a release package ships its module; a build tree holds modules
    // that are rebuilt while the game runs.
    if (!std::filesystem::is_regular_file(resourceRoot / "content-manifest.json", error)) {
        return GameModuleLoading::ShadowCopy;
    }
    const std::filesystem::path normalizedModule = std::filesystem::weakly_canonical(module, error);
    const std::filesystem::path normalizedRoot = std::filesystem::weakly_canonical(resourceRoot, error);
    if (error) {
        return GameModuleLoading::ShadowCopy;
    }
    const std::filesystem::path relative = normalizedModule.lexically_relative(normalizedRoot);
    const bool inside = !relative.empty() && relative.is_relative() && *relative.begin() != "..";
    return inside ? GameModuleLoading::InPlace : GameModuleLoading::ShadowCopy;
}
