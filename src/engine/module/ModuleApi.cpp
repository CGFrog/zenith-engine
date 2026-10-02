#include "ModuleApi.hpp"

std::string_view engineModuleBuildHash() {
    return ZENITH_MODULE_BUILD_HASH;
}

ModuleCompatibilityResult checkModuleCompatibility(std::string_view moduleName, uint32_t apiVersion,
                                                   std::string_view buildHash) {
    if (apiVersion != moduleApiVersion) {
        return {.error = "Game module '" + std::string(moduleName) + "' targets module API version " +
                         std::to_string(apiVersion) + "; this engine provides version " +
                         std::to_string(moduleApiVersion)};
    }
    if (buildHash != engineModuleBuildHash()) {
        return {.error = "Game module '" + std::string(moduleName) + "' was built as '" + std::string(buildHash) +
                         "' but this engine was built as '" + std::string(engineModuleBuildHash()) +
                         "'; rebuild the module with the engine's toolchain and configuration"};
    }
    return {.success = true};
}
