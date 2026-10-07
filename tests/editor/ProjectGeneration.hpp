#pragma once

// The project-template request the Project Hub tests and the generated-module runs generate projects from.

#include "editor/project/ProjectTemplate.hpp"

#include <filesystem>
#include <string>

// A request to generate a project named name under parent from the C++ game
// template, against this build of Zenith (ZENITH_TEMPLATE_DIR,
// ZENITH_ENGINE_BUILD_DIR, ZENITH_ASSET_DIR, ZENITH_EDITOR_CONFIGURATION).
inline ProjectTemplateRequest templateRequest(std::string name, const std::filesystem::path& parent) {
    return {.displayName = std::move(name),
            .parentDirectory = parent,
            .templateDirectory = std::filesystem::path(ZENITH_TEMPLATE_DIR) / cppGameTemplateName,
            .engineRoot = ZENITH_ENGINE_BUILD_DIR,
            .engineAssetRoot = ZENITH_ASSET_DIR,
            .buildConfiguration = ZENITH_EDITOR_CONFIGURATION};
}
