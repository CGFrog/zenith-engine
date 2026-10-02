#include "GraphicsValidation.hpp"

#include <SDL3/SDL_stdinc.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

GraphicsValidationOptions graphicsValidationOptionsFromEnvironment() {
    const char* value = SDL_getenv("ZENITH_VALIDATION_STRICT");
    const bool strict = value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    return {.strict = strict};
}

int validationExitCode(const GraphicsValidationReport& report, int applicationExitCode) {
    if (report.layerActive) {
        std::cerr << "Vulkan validation summary: " << report.errorCount << " error(s), " << report.warningCount
                  << " warning(s), " << report.performanceWarningCount << " performance warning(s), "
                  << report.loaderMessageCount << " loader message(s)"
                  << (report.synchronizationValidation ? ", synchronization validation on" : "") << ".\n";
    }
    if (!report.strict) {
        return applicationExitCode;
    }
    if (!report.layerActive) {
        std::cerr << "Strict validation skipped: VK_LAYER_KHRONOS_validation is unavailable.\n";
        return validationSkippedExitCode;
    }
    if (report.failingMessageCount() > 0) {
        std::cerr << "Strict validation failed.\n";
        return EXIT_FAILURE;
    }
    return applicationExitCode;
}
