#include "engine/graphics/GraphicsValidation.hpp"

#include "support/Doctest.hpp"

#include <SDL3/SDL_stdinc.h>

#include <cstdlib>
#include <iostream>

TEST_SUITE_BEGIN("GraphicsValidation");

namespace {

// The engine reads variables through SDL's cached environment, so tests edit
// that copy rather than the C runtime environment.
void setStrictVariable(const char* value) {
    SDL_Environment* environment = SDL_GetEnvironment();
    if (value == nullptr) {
        SDL_UnsetEnvironmentVariable(environment, "ZENITH_VALIDATION_STRICT");
    } else {
        SDL_SetEnvironmentVariable(environment, "ZENITH_VALIDATION_STRICT", value, true);
    }
}

TEST_CASE("GraphicsValidation: environment parsing") {
    setStrictVariable(nullptr);
    CHECK_MESSAGE(!graphicsValidationOptionsFromEnvironment().strict, "unset variable is not strict");
    setStrictVariable("0");
    CHECK_MESSAGE(!graphicsValidationOptionsFromEnvironment().strict, "'0' is not strict");
    setStrictVariable("1");
    CHECK_MESSAGE(graphicsValidationOptionsFromEnvironment().strict, "'1' requests strict validation");
    setStrictVariable(nullptr);
}

TEST_CASE("GraphicsValidation: non strict keeps application result") {
    const GraphicsValidationReport noisy{.layerActive = true, .errorCount = 3, .warningCount = 2};
    CHECK_MESSAGE(validationExitCode(noisy, EXIT_SUCCESS) == EXIT_SUCCESS, "messages are informational without strict");
    CHECK_MESSAGE(validationExitCode({}, EXIT_FAILURE) == EXIT_FAILURE, "application failure is preserved");
}

TEST_CASE("GraphicsValidation: strict policy") {
    CHECK_MESSAGE(validationExitCode({.strict = true}, EXIT_SUCCESS) == validationSkippedExitCode,
                  "missing layer skips a strict run");
    CHECK_MESSAGE(validationExitCode({.strict = true}, EXIT_FAILURE) == validationSkippedExitCode,
                  "missing layer skips even when initialization failed");
    CHECK_MESSAGE(validationExitCode({.strict = true, .layerActive = true}, EXIT_SUCCESS) == EXIT_SUCCESS,
                  "clean strict run succeeds");
    CHECK_MESSAGE(validationExitCode({.strict = true, .layerActive = true, .errorCount = 1}, EXIT_SUCCESS) ==
                      EXIT_FAILURE,
                  "an error fails a strict run");
    CHECK_MESSAGE(validationExitCode({.strict = true, .layerActive = true, .warningCount = 1}, EXIT_SUCCESS) ==
                      EXIT_FAILURE,
                  "a warning fails a strict run");
    CHECK_MESSAGE(validationExitCode({.strict = true, .layerActive = true, .performanceWarningCount = 4},
                                     EXIT_SUCCESS) == EXIT_SUCCESS,
                  "performance warnings are reported but not fatal");
    CHECK_MESSAGE(validationExitCode({.strict = true, .layerActive = true, .loaderMessageCount = 5}, EXIT_SUCCESS) ==
                      EXIT_SUCCESS,
                  "loader diagnostics are reported but not fatal");
    CHECK_MESSAGE(validationExitCode({.strict = true, .layerActive = true}, EXIT_FAILURE) == EXIT_FAILURE,
                  "a clean strict run keeps the application failure");
}
} // namespace

TEST_SUITE_END();
