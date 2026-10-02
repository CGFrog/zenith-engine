#include "engine/graphics/GraphicsValidation.hpp"

#include <SDL3/SDL_stdinc.h>

#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

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

void testEnvironmentParsing() {
    setStrictVariable(nullptr);
    expect(!graphicsValidationOptionsFromEnvironment().strict, "unset variable is not strict");
    setStrictVariable("0");
    expect(!graphicsValidationOptionsFromEnvironment().strict, "'0' is not strict");
    setStrictVariable("1");
    expect(graphicsValidationOptionsFromEnvironment().strict, "'1' requests strict validation");
    setStrictVariable(nullptr);
}

void testNonStrictKeepsApplicationResult() {
    const GraphicsValidationReport noisy{.layerActive = true, .errorCount = 3, .warningCount = 2};
    expect(validationExitCode(noisy, EXIT_SUCCESS) == EXIT_SUCCESS, "messages are informational without strict");
    expect(validationExitCode({}, EXIT_FAILURE) == EXIT_FAILURE, "application failure is preserved");
}

void testStrictPolicy() {
    expect(validationExitCode({.strict = true}, EXIT_SUCCESS) == validationSkippedExitCode,
           "missing layer skips a strict run");
    expect(validationExitCode({.strict = true}, EXIT_FAILURE) == validationSkippedExitCode,
           "missing layer skips even when initialization failed");
    expect(validationExitCode({.strict = true, .layerActive = true}, EXIT_SUCCESS) == EXIT_SUCCESS,
           "clean strict run succeeds");
    expect(validationExitCode({.strict = true, .layerActive = true, .errorCount = 1}, EXIT_SUCCESS) == EXIT_FAILURE,
           "an error fails a strict run");
    expect(validationExitCode({.strict = true, .layerActive = true, .warningCount = 1}, EXIT_SUCCESS) == EXIT_FAILURE,
           "a warning fails a strict run");
    expect(validationExitCode({.strict = true, .layerActive = true, .performanceWarningCount = 4}, EXIT_SUCCESS) ==
               EXIT_SUCCESS,
           "performance warnings are reported but not fatal");
    expect(validationExitCode({.strict = true, .layerActive = true, .loaderMessageCount = 5}, EXIT_SUCCESS) ==
               EXIT_SUCCESS,
           "loader diagnostics are reported but not fatal");
    expect(validationExitCode({.strict = true, .layerActive = true}, EXIT_FAILURE) == EXIT_FAILURE,
           "a clean strict run keeps the application failure");
}
} // namespace

int main() {
    testEnvironmentParsing();
    testNonStrictKeepsApplicationResult();
    testStrictPolicy();
    if (failures != 0) {
        std::cerr << failures << " graphics validation test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Graphics validation tests passed\n";
    return EXIT_SUCCESS;
}
